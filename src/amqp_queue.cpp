#include "amqp_queue.h"
#include "amqp_queue_p.h"
#include "amqp_exchange.h"

using namespace QAMQP;

#include <QCoreApplication>
#include <QDebug>
#include <QDataStream>
#include <QFile>

Queue::Queue(int channelNumber, Client *parent)
    : Channel(new QueuePrivate(this), parent)
{
    Q_D(Queue);
    d->init(channelNumber, parent);
}

Queue::~Queue()
{
    remove();
}

void Queue::onOpen()
{
    Q_D(Queue);
    if (d->delayedDeclare)
        d->declare();

    if (!d->delayedBindings.isEmpty()) {
        typedef QPair<QString, QString> BindingPair;
        foreach(BindingPair binding, d->delayedBindings)
            d->bind(binding.first, binding.second);
        d->delayedBindings.clear();
    }
}

void Queue::onClose()
{
    Q_D(Queue);
    d->remove(true, true);
}

Queue::QueueOptions Queue::option() const
{
    Q_D(const Queue);
    return d->options;
}

void Queue::setNoAck(bool noAck)
{
    Q_D(Queue);
    d->noAck = noAck;
}

bool Queue::noAck() const
{
    Q_D(const Queue);
    return d->noAck;
}

void Queue::declare()
{
    Q_D(Queue);
    declare(d->name, QueueOptions(Durable | AutoDelete));
}

void Queue::declare(const QString &name, QueueOptions options)
{
    Q_D(Queue);
    setName(name);
    d->options = options;
    d->declare();
}

void Queue::remove(bool ifUnused, bool ifEmpty, bool noWait)
{
    Q_D(Queue);
    d->remove(ifUnused, ifEmpty, noWait);
}

void Queue::purge()
{
    Q_D(Queue);
    d->purge();
}

void Queue::bind(const QString &exchangeName, const QString &key)
{
    Q_D(Queue);
    d->bind(exchangeName, key);
}

void Queue::bind(Exchange *exchange, const QString &key)
{
    Q_D(Queue);
    if (exchange)
        d->bind(exchange->name(), key);
}

void Queue::unbind(const QString &exchangeName, const QString &key)
{
    Q_D(Queue);
    d->unbind(exchangeName, key);
}

void Queue::unbind(Exchange *exchange, const QString &key)
{
    Q_D(Queue);
    if (exchange)
        d->unbind(exchange->name(), key);
}

void Queue::_q_content(const Frame::Content &frame)
{
    Q_D(Queue);
    d->_q_content(frame);
}

void Queue::_q_body(const Frame::ContentBody &frame)
{
    Q_D(Queue);
    d->_q_body(frame);
}

MessagePtr Queue::getMessage()
{
    Q_D(Queue);
    return d->messages_.dequeue();
}

bool Queue::hasMessage() const
{
    Q_D(const Queue);
    if (d->messages_.isEmpty())
        return false;

    const MessagePtr &q = d->messages_.head();
    return q->leftSize == 0;
}

void Queue::consume(ConsumeOptions options)
{
    Q_D(Queue);
    d->consume(options);
}

void Queue::setConsumerTag(const QString &consumerTag)
{
    Q_D(Queue);
    d->consumerTag = consumerTag;
}

QString Queue::consumerTag() const
{
    Q_D(const Queue);
    return d->consumerTag;
}

void Queue::get()
{
    Q_D(Queue);
    d->get();
}

void Queue::ack(const MessagePtr &message)
{
    Q_D(Queue);
    d->ack(message);
}

//////////////////////////////////////////////////////////////////////////


QueuePrivate::QueuePrivate(Queue * q)
    : ChannelPrivate(q),
      delayedDeclare(false),
      declared(false),
      noAck(true),
      recievingMessage(false)
{
}

QueuePrivate::~QueuePrivate()
{
}

bool QueuePrivate::_q_method(const Frame::Method &frame)
{
    Q_Q(Queue);
    if (ChannelPrivate::_q_method(frame))
        return true;

    if (frame.methodClass() == Frame::fcQueue) {
        switch (frame.id()) {
        case miDeclareOk:
            declareOk(frame);
            break;
        case miDelete:
            deleteOk(frame);
            break;
        case miBindOk:
            bindOk(frame);
            break;
        case miUnbindOk:
            unbindOk(frame);
            break;
        case miPurgeOk:
            deleteOk(frame);
            break;
        default:
            break;
        }

        return true;
    }

    if (frame.methodClass() == Frame::fcBasic) {
        switch(frame.id()) {
        case bmConsumeOk:
            consumeOk(frame);
            break;
        case bmDeliver:
            deliver(frame);
            break;
        case bmGetOk:
            getOk(frame);
            break;
        case bmGetEmpty:
            QMetaObject::invokeMethod(q, "empty");
            break;
        default:
            break;
        }
        return true;
    }

    return false;
}

void QueuePrivate::declareOk(const Frame::Method &frame)
{
    Q_Q(Queue);
    qDebug() << "Declared queue: " << name;
    declared = true;

    QByteArray data = frame.arguments();
    QDataStream stream(&data, QIODevice::ReadOnly);

    name = Frame::readField('s', stream).toString();
    qint32 messageCount = 0, consumerCount = 0;
    stream >> messageCount >> consumerCount;
    qDebug("Message count %d\nConsumer count: %d", messageCount, consumerCount);

    QMetaObject::invokeMethod(q, "declared");
}

void QueuePrivate::deleteOk(const Frame::Method &frame)
{
    Q_Q(Queue);
    qDebug() << "Deleted or purged queue: " << name;
    declared = false;

    QByteArray data = frame.arguments();
    QDataStream stream(&data, QIODevice::ReadOnly);
    qint32 messageCount = 0;
    stream >> messageCount;
    qDebug("Message count %d", messageCount);
    QMetaObject::invokeMethod(q, "removed");
}

void QueuePrivate::bindOk(const Frame::Method &frame)
{
    Q_UNUSED(frame)
    Q_Q(Queue);

    qDebug() << "Binded to queue: " << name;
    QMetaObject::invokeMethod(q, "binded", Q_ARG(bool, true));
}

void QueuePrivate::unbindOk(const Frame::Method &frame)
{
    Q_UNUSED(frame)
    Q_Q(Queue);

    qDebug() << "Unbinded queue: " << name;
    QMetaObject::invokeMethod(q, "binded", Q_ARG(bool, false));
}

void QueuePrivate::declare()
{
    if (!opened) {
        delayedDeclare = true;
        return;
    }

    Frame::Method frame(Frame::fcQueue, miDeclare);
    frame.setChannel(number);
    QByteArray arguments_;
    QDataStream out(&arguments_, QIODevice::WriteOnly);
    out << qint16(0); //reserver 1
    Frame::writeField('s', out, name);
    out << qint8(options);
    Frame::writeField('F', out, Frame::TableField());

    frame.setArguments(arguments_);
    sendFrame(frame);
    delayedDeclare = false;
}

void QueuePrivate::remove(bool ifUnused, bool ifEmpty, bool noWait)
{
    if (!declared)
        return;

    Frame::Method frame(Frame::fcQueue, miDelete);
    frame.setChannel(number);
    QByteArray arguments_;
    QDataStream out(&arguments_, QIODevice::WriteOnly);

    out << qint16(0); //reserver 1
    Frame::writeField('s', out, name);

    qint8 flag = 0;

    flag |= (ifUnused ? 0x1 : 0);
    flag |= (ifEmpty ? 0x2 : 0);
    flag |= (noWait ? 0x4 : 0);

    out << flag;

    frame.setArguments(arguments_);
    sendFrame(frame);
}

void QueuePrivate::purge()
{
    if (!opened)
        return;

    Frame::Method frame(Frame::fcQueue, miPurge);
    frame.setChannel(number);
    QByteArray arguments_;
    QDataStream out(&arguments_, QIODevice::WriteOnly);
    out << qint16(0); //reserver 1
    Frame::writeField('s', out, name);
    out << qint8(0); // no-wait
    frame.setArguments(arguments_);
    sendFrame(frame);
}

void QueuePrivate::bind(const QString & exchangeName, const QString &key)
{
    if (!opened) {
        delayedBindings.append(QPair<QString,QString>(exchangeName, key));
        return;
    }

    Frame::Method frame(Frame::fcQueue, miBind);
    frame.setChannel(number);
    QByteArray arguments_;
    QDataStream out(&arguments_, QIODevice::WriteOnly);
    out << qint16(0); //reserver 1
    Frame::writeField('s', out, name);
    Frame::writeField('s', out, exchangeName);
    Frame::writeField('s', out, key);
    out << qint8(0); // no-wait
    Frame::writeField('F', out, Frame::TableField());

    frame.setArguments(arguments_);
    sendFrame(frame);
}

void QueuePrivate::unbind(const QString &exchangeName, const QString &key)
{
    if (!opened)
        return;

    Frame::Method frame(Frame::fcQueue, miUnbind);
    frame.setChannel(number);
    QByteArray arguments_;
    QDataStream out(&arguments_, QIODevice::WriteOnly);
    out << qint16(0); //reserver 1
    Frame::writeField('s', out, name);
    Frame::writeField('s', out, exchangeName);
    Frame::writeField('s', out, key);
    Frame::writeField('F', out, Frame::TableField());

    frame.setArguments(arguments_);
    sendFrame(frame);
}

void QueuePrivate::get()
{
    if (!opened)
        return;

    Frame::Method frame(Frame::fcBasic, bmGet);
    frame.setChannel(number);
    QByteArray arguments_;
    QDataStream out(&arguments_, QIODevice::WriteOnly);
    out << qint16(0); //reserver 1
    Frame::writeField('s', out, name);
    out << qint8(noAck ? 1 : 0); // noAck

    frame.setArguments(arguments_);
    sendFrame(frame);
}

void QueuePrivate::getOk(const Frame::Method &frame)
{
    QByteArray data = frame.arguments();
    QDataStream in(&data, QIODevice::ReadOnly);

    qlonglong deliveryTag = Frame::readField('L',in).toLongLong();
    bool redelivered = Frame::readField('t',in).toBool();
    QString exchangeName = Frame::readField('s',in).toString();
    QString routingKey = Frame::readField('s',in).toString();

    Q_UNUSED(redelivered)

    MessagePtr newMessage = MessagePtr(new Message);
    newMessage->routeKey = routingKey;
    newMessage->exchangeName = exchangeName;
    newMessage->deliveryTag = deliveryTag;
    messages_.enqueue(newMessage);
}

void QueuePrivate::ack(const MessagePtr &Message)
{
    if (!opened)
        return;

    Frame::Method frame(Frame::fcBasic, bmAck);
    frame.setChannel(number);
    QByteArray arguments_;
    QDataStream out(&arguments_, QIODevice::WriteOnly);
    out << Message->deliveryTag; //reserver 1
    out << qint8(0); // noAck

    frame.setArguments(arguments_);
    sendFrame(frame);
}

void QueuePrivate::consume(Queue::ConsumeOptions options)
{
    if (!opened)
        return;

    Frame::Method frame(Frame::fcBasic, bmConsume);
    frame.setChannel(number);
    QByteArray arguments_;
    QDataStream out(&arguments_, QIODevice::WriteOnly);
    out << qint16(0); //reserver 1
    Frame::writeField('s', out, name);
    Frame::writeField('s', out, consumerTag);
    out << qint8(options); // no-wait
    Frame::writeField('F', out, Frame::TableField());

    frame.setArguments(arguments_);
    sendFrame(frame);
}

void QueuePrivate::consumeOk(const Frame::Method &frame)
{
    qDebug() << "Consume ok: " << name;
    declared = false;

    QByteArray data = frame.arguments();
    QDataStream stream(&data, QIODevice::ReadOnly);
    consumerTag = Frame::readField('s',stream).toString();
    qDebug("Consumer tag = %s", qPrintable(consumerTag));
}

void QueuePrivate::deliver(const Frame::Method &frame)
{
    QByteArray data = frame.arguments();
    QDataStream in(&data, QIODevice::ReadOnly);
    QString consumer_ = Frame::readField('s',in).toString();
    if (consumer_ != consumerTag)
        return;

    qlonglong deliveryTag = Frame::readField('L',in).toLongLong();
    bool redelivered = Frame::readField('t',in).toBool();
    QString exchangeName = Frame::readField('s',in).toString();
    QString routingKey = Frame::readField('s',in).toString();

    Q_UNUSED(redelivered)

    MessagePtr newMessage = MessagePtr(new Message);
    newMessage->routeKey = routingKey;
    newMessage->exchangeName = exchangeName;
    newMessage->deliveryTag = deliveryTag;
    messages_.enqueue(newMessage);
}

void QueuePrivate::_q_content(const Frame::Content &frame)
{
    Q_ASSERT(frame.channel() == number);
    if (frame.channel() != number)
        return;

    if (messages_.isEmpty()) {
        qErrnoWarning("Received content-header without method frame before");
        return;
    }

    MessagePtr &message = messages_.last();
    message->leftSize = frame.bodySize();
    QHash<int, QVariant>::ConstIterator i;
    for (i = frame.properties_.begin(); i != frame.properties_.end(); ++i)
        message->property[Message::MessageProperty(i.key())]= i.value();
}

void QueuePrivate::_q_body(const Frame::ContentBody &frame)
{
    Q_Q(Queue);
    Q_ASSERT(frame.channel() == number);
    if (frame.channel() != number)
        return;

    if (messages_.isEmpty()) {
        qErrnoWarning("Received content-body without method frame before");
        return;
    }

    MessagePtr &message = messages_.last();
    message->payload.append(frame.body());
    message->leftSize -= frame.body().size();

    if (message->leftSize == 0 && messages_.size() == 1)
        Q_EMIT q->messageReceived(q);
}
