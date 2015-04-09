#include <QEventLoop>
#include <QDataStream>
#include <QTimer>
#include <QDebug>

#include "qamqpexchange.h"
#include "qamqpexchange_p.h"
#include "qamqpqueue.h"
#include "qamqpglobal.h"
#include "qamqpclient.h"

QString QAmqpExchangePrivate::typeToString(QAmqpExchange::ExchangeType type)
{
    switch (type) {
    case QAmqpExchange::Direct: return QLatin1String("direct");
    case QAmqpExchange::FanOut: return QLatin1String("fanout");
    case QAmqpExchange::Topic: return QLatin1String("topic");
    case QAmqpExchange::Headers: return QLatin1String("headers");
    }

    return QLatin1String("direct");
}

QAmqpExchangePrivate::QAmqpExchangePrivate(QAmqpExchange *q)
    : QAmqpChannelPrivate(q),
      delayedDeclare(false),
      delayedRemove(false),
      exchangeState(ExchangeClosedState),
      nextDeliveryTag(0)
{
}

void QAmqpExchangePrivate::declare()
{
    Q_Q(QAmqpExchange);
    if (channelState != ChannelClosedState) {
        qAmqpDebug() << Q_FUNC_INFO << "Channel is closed, re-opening and delaying declare.";
        delayedDeclare = true;
        delayedRemove = false;
        needOpen = true;
        q->reopen();
        return;
    }

    if (exchangeState == ExchangeDeclaredState) {
        qAmqpDebug() << "Re-declaring exchange" << name;
    } else if (exchangeState != ExchangeUndeclaredState) {
        if (exchangeState != ExchangeDeclaringState) {
            qAmqpDebug() << "Delaying declare of exchange"
                         << name
                         << "(current state "
                         << exchangeState
                         << ")";
            delayedDeclare = true;
        } else {
            qAmqpDebug() << "Exchange" << name << "already declaring";
        }
        return;
    }

    if (name.isEmpty()) {
        qAmqpDebug() << Q_FUNC_INFO << "attempting to declare an unnamed exchange, aborting...";
        return;
    }

    qAmqpDebug() << "Declaring exchange" << name;
    newState(ExchangeDeclaringState);

    QAmqpMethodFrame frame(QAmqpFrame::Exchange, QAmqpExchangePrivate::miDeclare);
    frame.setChannel(channelNumber);

    QByteArray args;
    QDataStream stream(&args, QIODevice::WriteOnly);

    stream << qint16(0);    //reserved 1
    QAmqpFrame::writeAmqpField(stream, QAmqpMetaType::ShortString, name);
    QAmqpFrame::writeAmqpField(stream, QAmqpMetaType::ShortString, type);

    stream << qint8(options);
    QAmqpFrame::writeAmqpField(stream, QAmqpMetaType::Hash, arguments);

    frame.setArguments(args);
    sendFrame(frame);
    delayedDeclare = false;
}

bool QAmqpExchangePrivate::_q_method(const QAmqpMethodFrame &frame)
{
    Q_Q(QAmqpExchange);
    if (QAmqpChannelPrivate::_q_method(frame))
        return true;

    if (frame.methodClass() == QAmqpFrame::Basic) {
        switch (frame.id()) {
        case bmAck:
        case bmNack:
            handleAckOrNack(frame);
            break;
        case bmReturn: basicReturn(frame); break;

        default:
            break;
        }

        return true;
    }

    if (frame.methodClass() == QAmqpFrame::Confirm) {
        if (frame.id() == cmConfirmOk) {
            Q_EMIT q->confirmsEnabled();
            return true;
        }
    }

    if (frame.methodClass() == QAmqpFrame::Exchange) {
        switch (frame.id()) {
        case miDeclareOk: declareOk(frame); break;
        case miDeleteOk: deleteOk(frame); break;

        default:
            break;
        }

        return true;
    }

    return false;
}

void QAmqpExchangePrivate::declareOk(const QAmqpMethodFrame &frame)
{
    Q_UNUSED(frame)

    Q_Q(QAmqpExchange);
    qAmqpDebug() << "declared exchange: " << name;
    newState(ExchangeDeclaredState);
    Q_EMIT q->declared();
    if (delayedRemove)
        q->remove(removeOptions);
}

void QAmqpExchangePrivate::deleteOk(const QAmqpMethodFrame &frame)
{
    Q_UNUSED(frame)

    Q_Q(QAmqpExchange);
    qAmqpDebug() << "deleted exchange: " << name;
    newState(ExchangeUndeclaredState);
    Q_EMIT q->removed();
    if (delayedDeclare)
        declare();
}

void QAmqpExchangePrivate::_q_disconnected()
{
    QAmqpChannelPrivate::_q_disconnected();
    qAmqpDebug() << "exchange " << name << " disconnected";
    delayedDeclare = false;
    newState(ExchangeClosedState);
}

void QAmqpExchangePrivate::basicReturn(const QAmqpMethodFrame &frame)
{
    Q_Q(QAmqpExchange);
    QByteArray data = frame.arguments();
    QDataStream stream(&data, QIODevice::ReadOnly);

    quint16 replyCode;
    stream >> replyCode;
    QString replyText = QAmqpFrame::readAmqpField(stream, QAmqpMetaType::ShortString).toString();
    QString exchangeName = QAmqpFrame::readAmqpField(stream, QAmqpMetaType::ShortString).toString();
    QString routingKey = QAmqpFrame::readAmqpField(stream, QAmqpMetaType::ShortString).toString();

    QAMQP::Error checkError = static_cast<QAMQP::Error>(replyCode);
    if (checkError != QAMQP::NoError) {
        error = checkError;
        errorString = qPrintable(replyText);
        Q_EMIT q->error(error);
    }

    qAmqpDebug(">> replyCode: %d", replyCode);
    qAmqpDebug(">> replyText: %s", qPrintable(replyText));
    qAmqpDebug(">> exchangeName: %s", qPrintable(exchangeName));
    qAmqpDebug(">> routingKey: %s", qPrintable(routingKey));
}

void QAmqpExchangePrivate::handleAckOrNack(const QAmqpMethodFrame &frame)
{
    Q_Q(QAmqpExchange);
    QByteArray data = frame.arguments();
    QDataStream stream(&data, QIODevice::ReadOnly);

    qlonglong deliveryTag =
        QAmqpFrame::readAmqpField(stream, QAmqpMetaType::LongLongUint).toLongLong();
    bool multiple = QAmqpFrame::readAmqpField(stream, QAmqpMetaType::Boolean).toBool();
    if (frame.id() == QAmqpExchangePrivate::bmAck) {
        if (deliveryTag == 0) {
            unconfirmedDeliveryTags.clear();
        } else {
            int idx = unconfirmedDeliveryTags.indexOf(deliveryTag);
            if (idx == -1) {
                return;
            }

            if (multiple) {
                unconfirmedDeliveryTags.remove(0, idx + 1);
            } else {
                unconfirmedDeliveryTags.remove(idx);
            }
        }

        if (unconfirmedDeliveryTags.isEmpty())
            Q_EMIT q->allMessagesDelivered();

    } else {
        qAmqpDebug() << "nacked(" << deliveryTag << "), multiple=" << multiple;
    }
}

/*! Report and change state. */
void QAmqpExchangePrivate::newState(ExchangeState state)
{
    qAmqpDebug() << "Exchange state: "
                 << exchangeState
                 << " -> "
                 << state;
    exchangeState = state;
}

void QAmqpExchangePrivate::newState(ChannelState state)
{
    QAmqpChannelPrivate::newState(state);
    if (state == QAmqpChannelPrivate::ChannelClosedState)
        newState(ExchangeClosedState);
}

QDebug operator<<(QDebug dbg, QAmqpExchangePrivate::ExchangeState s)
{
    switch(s) {
    case QAmqpExchangePrivate::ExchangeClosedState:
        dbg << "ExchangeClosedState";
        break;
    case QAmqpExchangePrivate::ExchangeUndeclaredState:
        dbg << "ExchangeUndeclaredState";
        break;
    case QAmqpExchangePrivate::ExchangeDeclaringState:
        dbg << "ExchangeDeclaringState";
        break;
    case QAmqpExchangePrivate::ExchangeDeclaredState:
        dbg << "ExchangeDeclaredState";
        break;
    case QAmqpExchangePrivate::ExchangeRemovingState:
        dbg << "ExchangeRemovingState";
        break;
    default:
        dbg << "{UNKNOWN EXCHANGE STATE}";
    }
    return dbg;
}

//////////////////////////////////////////////////////////////////////////

QAmqpExchange::QAmqpExchange(int channelNumber, QAmqpClient *parent)
    : QAmqpChannel(new QAmqpExchangePrivate(this), parent)
{
    Q_D(QAmqpExchange);
    d->init(channelNumber, parent);
}

QAmqpExchange::~QAmqpExchange()
{
}

void QAmqpExchange::channelOpened()
{
    Q_D(QAmqpExchange);
    qAmqpDebug() << "Channel open";

    if (!d->delayedDeclare && (name().isEmpty() || name().startsWith("amq."))) {
        /* Nameless exchange, we should consider this declared by default */
        qAmqpDebug() << "Automatically declaring built-in exchange:" << name();
        d->newState(QAmqpExchangePrivate::ExchangeDeclaredState);
        Q_EMIT declared();
        return;
    } else {
        d->newState(QAmqpExchangePrivate::ExchangeUndeclaredState);
    }

    if (d->delayedRemove)
        remove(d->removeOptions);
    else if (d->delayedDeclare)
        d->declare();
    else
        qAmqpDebug() << "No delayed declare pending for" << name();
}

void QAmqpExchange::channelClosed()
{
    Q_D(QAmqpExchange);
    qAmqpDebug() << "Channel closed";

    if (!(name().isEmpty() || name().startsWith("amq.")))
        d->delayedDeclare = (d->exchangeState
                == QAmqpExchangePrivate::ExchangeDeclaredState)
            || (d->exchangeState
                    == QAmqpExchangePrivate::ExchangeDeclaringState);
    d->newState(QAmqpExchangePrivate::ExchangeClosedState);
}

QAmqpExchange::ExchangeOptions QAmqpExchange::options() const
{
    Q_D(const QAmqpExchange);
    return d->options;
}

QString QAmqpExchange::type() const
{
    Q_D(const QAmqpExchange);
    return d->type;
}

bool QAmqpExchange::isDeclared() const
{
    Q_D(const QAmqpExchange);
    return (d->exchangeState == QAmqpExchangePrivate::ExchangeDeclaredState);
}

void QAmqpExchange::declare(ExchangeType type, ExchangeOptions options, const QAmqpTable &args)
{
    declare(QAmqpExchangePrivate::typeToString(type), options, args);
}

void QAmqpExchange::declare(const QString &type, ExchangeOptions options, const QAmqpTable &args)
{
    Q_D(QAmqpExchange);
    d->type = type;
    d->options = options;
    d->arguments = args;
    d->declare();
}

void QAmqpExchange::remove(int options)
{
    Q_D(QAmqpExchange);
    if (!isOpen()) {
        qAmqpDebug() << Q_FUNC_INFO << "Channel is closed, re-opening and delaying remove.";
        d->delayedDeclare = false;
        d->delayedRemove = true;
        d->needOpen = true;
        d->removeOptions = options;
        reopen();
        return;
    }

    QAmqpMethodFrame frame(QAmqpFrame::Exchange, QAmqpExchangePrivate::miDelete);
    frame.setChannel(d->channelNumber);

    QByteArray arguments;
    QDataStream stream(&arguments, QIODevice::WriteOnly);

    stream << qint16(0);    //reserved 1
    QAmqpFrame::writeAmqpField(stream, QAmqpMetaType::ShortString, d->name);
    stream << qint8(options);

    frame.setArguments(arguments);
    d->sendFrame(frame);
    d->delayedRemove = false;
}

void QAmqpExchange::publish(const QString &message, const QString &routingKey,
                            const QAmqpMessage::PropertyHash &properties, int publishOptions)
{
    publish(message.toUtf8(), routingKey, QLatin1String("text.plain"),
            QAmqpTable(), properties, publishOptions);
}

void QAmqpExchange::publish(const QByteArray &message, const QString &routingKey,
                            const QString &mimeType, const QAmqpMessage::PropertyHash &properties,
                            int publishOptions)
{
    publish(message, routingKey, mimeType, QAmqpTable(), properties, publishOptions);
}

void QAmqpExchange::publish(const QByteArray &message, const QString &routingKey,
                            const QString &mimeType, const QAmqpTable &headers,
                            const QAmqpMessage::PropertyHash &properties, int publishOptions)
{
    Q_D(QAmqpExchange);
    if (d->nextDeliveryTag > 0) {
        d->unconfirmedDeliveryTags.append(d->nextDeliveryTag);
        d->nextDeliveryTag++;
    }

    QAmqpMethodFrame frame(QAmqpFrame::Basic, QAmqpExchangePrivate::bmPublish);
    frame.setChannel(d->channelNumber);

    QByteArray arguments;
    QDataStream out(&arguments, QIODevice::WriteOnly);

    out << qint16(0);   //reserved 1
    QAmqpFrame::writeAmqpField(out, QAmqpMetaType::ShortString, d->name);
    QAmqpFrame::writeAmqpField(out, QAmqpMetaType::ShortString, routingKey);
    out << qint8(publishOptions);

    frame.setArguments(arguments);
    d->sendFrame(frame);

    QAmqpContentFrame content(QAmqpFrame::Basic);
    content.setChannel(d->channelNumber);
    content.setProperty(QAmqpMessage::ContentType, mimeType);
    content.setProperty(QAmqpMessage::ContentEncoding, "utf-8");
    content.setProperty(QAmqpMessage::Headers, headers);
    content.setProperty(QAmqpMessage::MessageId, "0");

    QAmqpMessage::PropertyHash::ConstIterator it;
    QAmqpMessage::PropertyHash::ConstIterator itEnd = properties.constEnd();
    for (it = properties.constBegin(); it != itEnd; ++it)
        content.setProperty(it.key(), it.value());
    content.setBodySize(message.size());
    d->sendFrame(content);

    int fullSize = message.size();
    for (int sent = 0; sent < fullSize; sent += (d->client->frameMax() - 7)) {
        QAmqpContentBodyFrame body;
        QByteArray partition = message.mid(sent, (d->client->frameMax() - 7));
        body.setChannel(d->channelNumber);
        body.setBody(partition);
        d->sendFrame(body);
    }
}

void QAmqpExchange::enableConfirms(bool noWait)
{
    Q_D(QAmqpExchange);
    QAmqpMethodFrame frame(QAmqpFrame::Confirm, QAmqpExchangePrivate::cmConfirm);
    frame.setChannel(d->channelNumber);

    QByteArray arguments;
    QDataStream stream(&arguments, QIODevice::WriteOnly);
    stream << qint8(noWait ? 1 : 0);

    frame.setArguments(arguments);
    d->sendFrame(frame);

    // for tracking acks and nacks
    if (d->nextDeliveryTag == 0) d->nextDeliveryTag = 1;
}

bool QAmqpExchange::waitForConfirms(int msecs)
{
    Q_D(QAmqpExchange);

    QEventLoop loop;
    connect(this, SIGNAL(allMessagesDelivered()), &loop, SLOT(quit()));
    QTimer::singleShot(msecs, &loop, SLOT(quit()));
    loop.exec();

    return (d->unconfirmedDeliveryTags.isEmpty());
}
