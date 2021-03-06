#include "./serialport.h"

#ifdef WIN32
  #define strncasecmp strnicmp
#else
  #include "./read-poller.h"
#endif

struct _WriteQueue {
  const int _fd;  // the fd that is associated with this write queue
  QueuedWrite _write_queue;
  uv_mutex_t _write_queue_mutex;
  _WriteQueue *_next;

  _WriteQueue(const int fd) : _fd(fd), _write_queue(), _next(NULL) {
    uv_mutex_init(&_write_queue_mutex);
  }

  void lock() { uv_mutex_lock(&_write_queue_mutex); }
  void unlock() { uv_mutex_unlock(&_write_queue_mutex); }

  QueuedWrite &get() { return _write_queue; }
};

static _WriteQueue *write_queues = NULL;

static _WriteQueue *qForFD(const int fd) {
  _WriteQueue *q = write_queues;
  while (q != NULL) {
    if (q->_fd == fd) {
      return q;
    }
    q = q->_next;
  }
  return NULL;
}

static _WriteQueue *newQForFD(const int fd) {
  _WriteQueue *q = qForFD(fd);

  if (q == NULL) {
    if (write_queues == NULL) {
      write_queues = new _WriteQueue(fd);
      return write_queues;
    } else {
      q = write_queues;
      while (q->_next != NULL) {
        q = q->_next;
      }
      q->_next = new _WriteQueue(fd);
      return q->_next;
    }
  }

  return q;
}

static void deleteQForFD(const int fd) {
  if (write_queues == NULL)
    return;

  _WriteQueue *q = write_queues;
  if (write_queues->_fd == fd) {
    write_queues = write_queues->_next;
    delete q;

    return;
  }

  while (q->_next != NULL) {
    if (q->_next->_fd == fd) {
      _WriteQueue *out_q = q->_next;
      q->_next = q->_next->_next;
      delete out_q;

      return;
    }
    q = q->_next;
  }

  // It wasn't found...
}

v8::Local<v8::Value> getValueFromObject(v8::Local<v8::Object> options, std::string key) {
  v8::Local<v8::String> v8str = Nan::New<v8::String>(key).ToLocalChecked();
  return Nan::Get(options, v8str).ToLocalChecked();
}

int getIntFromObject(v8::Local<v8::Object> options, std::string key) {
  return Nan::To<v8::Int32>(getValueFromObject(options, key)).ToLocalChecked()->Value();
}

bool getBoolFromObject(v8::Local<v8::Object> options, std::string key) {
  return Nan::To<v8::Boolean>(getValueFromObject(options, key)).ToLocalChecked()->Value();
}

v8::Local<v8::String> getStringFromObj(v8::Local<v8::Object> options, std::string key) {
  return Nan::To<v8::String>(getValueFromObject(options, key)).ToLocalChecked();
}

double getDoubleFromObject(v8::Local<v8::Object> options, std::string key) {
  return Nan::To<double>(getValueFromObject(options, key)).FromMaybe(0);
}

NAN_METHOD(Open) {
  // path
  if (!info[0]->IsString()) {
    Nan::ThrowTypeError("First argument must be a string");
    return;
  }
  v8::String::Utf8Value path(info[0]->ToString());

  // options
  if (!info[1]->IsObject()) {
    Nan::ThrowTypeError("Second argument must be an object");
    return;
  }
  v8::Local<v8::Object> options = info[1]->ToObject();

  // callback
  if (!info[2]->IsFunction()) {
    Nan::ThrowTypeError("Third argument must be a function");
    return;
  }

  OpenBaton* baton = new OpenBaton();
  memset(baton, 0, sizeof(OpenBaton));
  strcpy(baton->path, *path);
  baton->baudRate = getIntFromObject(options, "baudRate");
  baton->dataBits = getIntFromObject(options, "dataBits");
  baton->parity = ToParityEnum(getStringFromObj(options, "parity"));
  baton->stopBits = ToStopBitEnum(getDoubleFromObject(options, "stopBits"));
  baton->rtscts = getBoolFromObject(options, "rtscts");
  baton->xon = getBoolFromObject(options, "xon");
  baton->xoff = getBoolFromObject(options, "xoff");
  baton->xany = getBoolFromObject(options, "xany");
  baton->hupcl = getBoolFromObject(options, "hupcl");
  baton->lock = getBoolFromObject(options, "lock");
  baton->callback.Reset(info[2].As<v8::Function>());

  #ifndef WIN32
    baton->vmin = getIntFromObject(options, "vmin");
    baton->vtime = getIntFromObject(options, "vtime");
  #endif

  uv_work_t* req = new uv_work_t();
  req->data = baton;

  uv_queue_work(uv_default_loop(), req, EIO_Open, (uv_after_work_cb)EIO_AfterOpen);
}

void EIO_AfterOpen(uv_work_t* req) {
  Nan::HandleScope scope;

  OpenBaton* data = static_cast<OpenBaton*>(req->data);

  v8::Local<v8::Value> argv[2];
  if (data->errorString[0]) {
    argv[0] = v8::Exception::Error(Nan::New<v8::String>(data->errorString).ToLocalChecked());
    argv[1] = Nan::Undefined();
  } else {
    argv[0] = Nan::Null();
    argv[1] = Nan::New<v8::Int32>(data->result);

    int fd = Nan::To<v8::Int32>(argv[1]).ToLocalChecked()->Value();
    newQForFD(fd);
  }

  data->callback.Call(2, argv);
  delete data;
  delete req;
}

NAN_METHOD(Update) {
  // file descriptor
  if (!info[0]->IsInt32()) {
    Nan::ThrowTypeError("First argument must be an int");
    return;
  }
  int fd = Nan::To<v8::Int32>(info[0]).ToLocalChecked()->Value();

  // options
  if (!info[1]->IsObject()) {
    Nan::ThrowTypeError("Second argument must be an object");
    return;
  }
  v8::Local<v8::Object> options = info[1]->ToObject();

  if (!Nan::Has(options, Nan::New<v8::String>("baudRate").ToLocalChecked()).FromMaybe(false)) {
    Nan::ThrowTypeError("\"baudRate\" must be set on options object");
    return;
  }

  // callback
  if (!info[2]->IsFunction()) {
    Nan::ThrowTypeError("Third argument must be a function");
    return;
  }

  ConnectionOptionsBaton* baton = new ConnectionOptionsBaton();
  memset(baton, 0, sizeof(ConnectionOptionsBaton));

  baton->fd = fd;
  baton->baudRate = getIntFromObject(options, "baudRate");
  baton->callback.Reset(info[2].As<v8::Function>());

  uv_work_t* req = new uv_work_t();
  req->data = baton;

  uv_queue_work(uv_default_loop(), req, EIO_Update, (uv_after_work_cb)EIO_AfterUpdate);
}

void EIO_AfterUpdate(uv_work_t* req) {
  Nan::HandleScope scope;

  ConnectionOptionsBaton* data = static_cast<ConnectionOptionsBaton*>(req->data);

  v8::Local<v8::Value> argv[1];
  if (data->errorString[0]) {
    argv[0] = v8::Exception::Error(Nan::New<v8::String>(data->errorString).ToLocalChecked());
  } else {
    argv[0] = Nan::Null();
  }

  data->callback.Call(1, argv);

  delete data;
  delete req;
}

NAN_METHOD(Write) {
  // file descriptor
  if (!info[0]->IsInt32()) {
    Nan::ThrowTypeError("First argument must be an int");
    return;
  }
  int fd = Nan::To<v8::Int32>(info[0]).ToLocalChecked()->Value();

  // buffer
  if (!info[1]->IsObject() || !node::Buffer::HasInstance(info[1])) {
    Nan::ThrowTypeError("Second argument must be a buffer");
    return;
  }
  v8::Local<v8::Object> buffer = info[1]->ToObject();
  char* bufferData = node::Buffer::Data(buffer);
  size_t bufferLength = node::Buffer::Length(buffer);

  // callback
  if (!info[2]->IsFunction()) {
    Nan::ThrowTypeError("Third argument must be a function");
    return;
  }

  WriteBaton* data = new WriteBaton();
  memset(data, 0, sizeof(WriteBaton));
  data->fd = fd;
  data->buffer.Reset(buffer);
  data->bufferData = bufferData;
  data->bufferLength = bufferLength;
  data->offset = 0;
  data->callback.Reset(info[2].As<v8::Function>());

  QueuedWrite* queuedWrite = new QueuedWrite();
  memset(queuedWrite, 0, sizeof(QueuedWrite));
  queuedWrite->baton = data;
  queuedWrite->req.data = queuedWrite;

  _WriteQueue *q = qForFD(fd);
  if (!q) {
    v8::Local<v8::Value> argv[1];
    argv[0] = v8::Exception::Error(Nan::New<v8::String>("There's no write queue for file descriptor").ToLocalChecked());
    data->callback.Call(1, argv);
    delete data;
    return;
  }

  q->lock();
  QueuedWrite &write_queue = q->get();
  bool empty = write_queue.empty();

  write_queue.insert_tail(queuedWrite);

  if (empty) {
    uv_queue_work(uv_default_loop(), &queuedWrite->req, EIO_Write, (uv_after_work_cb)EIO_AfterWrite);
  }
  q->unlock();
}

void EIO_AfterWrite(uv_work_t* req) {
  Nan::HandleScope scope;

  QueuedWrite* queuedWrite = static_cast<QueuedWrite*>(req->data);
  WriteBaton* data = static_cast<WriteBaton*>(queuedWrite->baton);

  v8::Local<v8::Value> argv[1];
  if (data->errorString[0]) {
    argv[0] = v8::Exception::Error(Nan::New<v8::String>(data->errorString).ToLocalChecked());
  } else {
    argv[0] = Nan::Null();
  }

  if (data->offset < data->bufferLength && !data->errorString[0]) {
    // We're not done with this baton, so throw it right back onto the queue.
    // Don't re-push the write in the event loop if there was an error; because same error could occur again!
    // TODO: Add a uv_poll here for unix...
    uv_queue_work(uv_default_loop(), req, EIO_Write, (uv_after_work_cb)EIO_AfterWrite);
    return;
  }

  // throwing errors instead of returning them at this point is rude
  int fd = data->fd;
  _WriteQueue *q = qForFD(fd);
  if (!q) {
    Nan::ThrowTypeError("There's no write queue for that file descriptor (after write)");
    return;
  }

  q->lock();
  QueuedWrite &write_queue = q->get();

  // remove this one from the list
  queuedWrite->remove();

  data->callback.Call(1, argv);

  // If there are any left, start a new thread to write the next one.
  if (!write_queue.empty()) {
    // Always pull the next work item from the head of the queue
    QueuedWrite* nextQueuedWrite = write_queue.next;
    uv_queue_work(uv_default_loop(), &nextQueuedWrite->req, EIO_Write, (uv_after_work_cb)EIO_AfterWrite);
  }
  q->unlock();

  data->buffer.Reset();
  delete data;
  delete queuedWrite;
}

#ifdef WIN32
NAN_METHOD(Read) {
  // file descriptor
  if (!info[0]->IsInt32()) {
    Nan::ThrowTypeError("First argument must be an int");
    return;
  }
  int fd = Nan::To<v8::Int32>(info[0]).ToLocalChecked()->Value();

  // buffer
  if (!info[1]->IsObject() || !node::Buffer::HasInstance(info[1])) {
    Nan::ThrowTypeError("Second argument must be a buffer");
    return;
  }

  // offset
  if (!info[2]->IsInt32()) {
    Nan::ThrowTypeError("Third argument must be an int");
    return;
  }
  int offset = Nan::To<v8::Int32>(info[2]).ToLocalChecked()->Value();
  
  // bytes to read
  if (!info[3]->IsInt32()) {
    Nan::ThrowTypeError("Fourth argument must be an int");
    return;
  }
  int bytesToRead = Nan::To<v8::Int32>(info[3]).ToLocalChecked()->Value();

  // callback
  if (!info[4]->IsFunction()) {
    Nan::ThrowTypeError("Fifth argument must be a function");
    return;
  }

  ReadBaton* data = new ReadBaton();
  memset(data, 0, sizeof(ReadBaton));
  data->fd = fd;
  data->offset = offset;
  data->bytesToRead = bytesToRead;
  v8::Local<v8::Object> buffer = info[1]->ToObject();
  data->bufferData = node::Buffer::Data(buffer);
  data->callback.Reset(info[4].As<v8::Function>());

  uv_work_t* req = new uv_work_t();
  req->data = data;
  uv_queue_work(uv_default_loop(), req, EIO_Read, (uv_after_work_cb)EIO_AfterRead);
}

void EIO_AfterRead(uv_work_t* req) {
  Nan::HandleScope scope;
  ReadBaton* data = static_cast<ReadBaton*>(req->data);

  v8::Local<v8::Value> argv[2];
    
  if (data->errorString[0]) {
    argv[0] = Nan::Error(data->errorString);
    argv[1] = Nan::Undefined();
  } else {
    argv[0] = Nan::Null();
    argv[1] = Nan::New<v8::Integer>((int)data->bytesRead);
  }

  data->callback.Call(2, argv);

  delete data;
  delete req;
}
#endif

NAN_METHOD(Close) {
  // file descriptor
  if (!info[0]->IsInt32()) {
    Nan::ThrowTypeError("First argument must be an int");
    return;
  }

  // callback
  if (!info[1]->IsFunction()) {
    Nan::ThrowTypeError("Second argument must be a function");
    return;
  }

  VoidBaton* baton = new VoidBaton();
  memset(baton, 0, sizeof(VoidBaton));
  baton->fd = Nan::To<v8::Int32>(info[0]).ToLocalChecked()->Value();
  baton->callback.Reset(info[1].As<v8::Function>());

  uv_work_t* req = new uv_work_t();
  req->data = baton;
  uv_queue_work(uv_default_loop(), req, EIO_Close, (uv_after_work_cb)EIO_AfterClose);
}

void EIO_AfterClose(uv_work_t* req) {
  Nan::HandleScope scope;
  VoidBaton* data = static_cast<VoidBaton*>(req->data);

  v8::Local<v8::Value> argv[1];
  if (data->errorString[0]) {
    argv[0] = v8::Exception::Error(Nan::New<v8::String>(data->errorString).ToLocalChecked());
  } else {
    argv[0] = Nan::Null();

    // We don't have an error, so clean up the write queue for that fd
    _WriteQueue *q = qForFD(data->fd);
    if (q) {
      q->lock();
      QueuedWrite &write_queue = q->get();
      while (!write_queue.empty()) {
        QueuedWrite *del_q = write_queue.next;
        del_q->baton->buffer.Reset();
        del_q->remove();
      }
      q->unlock();
      deleteQForFD(data->fd);
    }
  }
  data->callback.Call(1, argv);

  delete data;
  delete req;
}

NAN_METHOD(List) {
  // callback
  if (!info[0]->IsFunction()) {
    Nan::ThrowTypeError("First argument must be a function");
    return;
  }

  ListBaton* baton = new ListBaton();
  strcpy(baton->errorString, "");
  baton->callback.Reset(info[0].As<v8::Function>());

  uv_work_t* req = new uv_work_t();
  req->data = baton;
  uv_queue_work(uv_default_loop(), req, EIO_List, (uv_after_work_cb)EIO_AfterList);
}

void setIfNotEmpty(v8::Local<v8::Object> item, std::string key, const char *value) {
  v8::Local<v8::String> v8key = Nan::New<v8::String>(key).ToLocalChecked();
  if (strlen(value) > 0) {
    Nan::Set(item, v8key, Nan::New<v8::String>(value).ToLocalChecked());
  } else {
    Nan::Set(item, v8key, Nan::Undefined());
  }
}

void EIO_AfterList(uv_work_t* req) {
  Nan::HandleScope scope;

  ListBaton* data = static_cast<ListBaton*>(req->data);

  v8::Local<v8::Value> argv[2];
  if (data->errorString[0]) {
    argv[0] = v8::Exception::Error(Nan::New<v8::String>(data->errorString).ToLocalChecked());
    argv[1] = Nan::Undefined();
  } else {
    v8::Local<v8::Array> results = Nan::New<v8::Array>();
    int i = 0;
    for (std::list<ListResultItem*>::iterator it = data->results.begin(); it != data->results.end(); ++it, i++) {
      v8::Local<v8::Object> item = Nan::New<v8::Object>();

      setIfNotEmpty(item, "comName", (*it)->comName.c_str());
      setIfNotEmpty(item, "manufacturer", (*it)->manufacturer.c_str());
      setIfNotEmpty(item, "serialNumber", (*it)->serialNumber.c_str());
      setIfNotEmpty(item, "pnpId", (*it)->pnpId.c_str());
      setIfNotEmpty(item, "locationId", (*it)->locationId.c_str());
      setIfNotEmpty(item, "vendorId", (*it)->vendorId.c_str());
      setIfNotEmpty(item, "productId", (*it)->productId.c_str());

      Nan::Set(results, i, item);
    }
    argv[0] = Nan::Null();
    argv[1] = results;
  }
  data->callback.Call(2, argv);

  for (std::list<ListResultItem*>::iterator it = data->results.begin(); it != data->results.end(); ++it) {
    delete *it;
  }
  delete data;
  delete req;
}

NAN_METHOD(Flush) {
  // file descriptor
  if (!info[0]->IsInt32()) {
    Nan::ThrowTypeError("First argument must be an int");
    return;
  }
  int fd = Nan::To<v8::Int32>(info[0]).ToLocalChecked()->Value();

  // callback
  if (!info[1]->IsFunction()) {
    Nan::ThrowTypeError("Second argument must be a function");
    return;
  }
  v8::Local<v8::Function> callback = info[1].As<v8::Function>();

  VoidBaton* baton = new VoidBaton();
  memset(baton, 0, sizeof(VoidBaton));
  baton->fd = fd;
  baton->callback.Reset(callback);

  uv_work_t* req = new uv_work_t();
  req->data = baton;
  uv_queue_work(uv_default_loop(), req, EIO_Flush, (uv_after_work_cb)EIO_AfterFlush);
}

void EIO_AfterFlush(uv_work_t* req) {
  Nan::HandleScope scope;

  VoidBaton* data = static_cast<VoidBaton*>(req->data);

  v8::Local<v8::Value> argv[1];

  if (data->errorString[0]) {
    argv[0] = v8::Exception::Error(Nan::New<v8::String>(data->errorString).ToLocalChecked());
  } else {
    argv[0] = Nan::Null();
  }

  data->callback.Call(1, argv);

  delete data;
  delete req;
}

NAN_METHOD(Set) {
  // file descriptor
  if (!info[0]->IsInt32()) {
    Nan::ThrowTypeError("First argument must be an int");
    return;
  }
  int fd = Nan::To<v8::Int32>(info[0]).ToLocalChecked()->Value();

  // options
  if (!info[1]->IsObject()) {
    Nan::ThrowTypeError("Second argument must be an object");
    return;
  }
  v8::Local<v8::Object> options = info[1]->ToObject();

  // callback
  if (!info[2]->IsFunction()) {
    Nan::ThrowTypeError("Third argument must be a function");
    return;
  }
  v8::Local<v8::Function> callback = info[2].As<v8::Function>();

  SetBaton* baton = new SetBaton();
  memset(baton, 0, sizeof(SetBaton));
  baton->fd = fd;
  baton->callback.Reset(callback);
  baton->brk = getBoolFromObject(options, "brk");
  baton->rts = getBoolFromObject(options, "rts");
  baton->cts = getBoolFromObject(options, "cts");
  baton->dtr = getBoolFromObject(options, "dtr");
  baton->dsr = getBoolFromObject(options, "dsr");

  uv_work_t* req = new uv_work_t();
  req->data = baton;
  uv_queue_work(uv_default_loop(), req, EIO_Set, (uv_after_work_cb)EIO_AfterSet);
}

void EIO_AfterSet(uv_work_t* req) {
  Nan::HandleScope scope;

  SetBaton* data = static_cast<SetBaton*>(req->data);

  v8::Local<v8::Value> argv[1];

  if (data->errorString[0]) {
    argv[0] = v8::Exception::Error(Nan::New<v8::String>(data->errorString).ToLocalChecked());
  } else {
    argv[0] = Nan::Null();
  }
  data->callback.Call(1, argv);

  delete data;
  delete req;
}

NAN_METHOD(Get) {
  // file descriptor
  if (!info[0]->IsInt32()) {
    Nan::ThrowTypeError("First argument must be an int");
    return;
  }
  int fd = Nan::To<v8::Int32>(info[0]).ToLocalChecked()->Value();

  // callback
  if (!info[1]->IsFunction()) {
    Nan::ThrowTypeError("Second argument must be a function");
    return;
  }

  GetBaton* baton = new GetBaton();
  memset(baton, 0, sizeof(GetBaton));
  baton->fd = fd;
  baton->cts = false;
  baton->dsr = false;
  baton->dcd = false;
  baton->callback.Reset(info[1].As<v8::Function>());

  uv_work_t* req = new uv_work_t();
  req->data = baton;
  uv_queue_work(uv_default_loop(), req, EIO_Get, (uv_after_work_cb)EIO_AfterGet);
}

void EIO_AfterGet(uv_work_t* req) {
  Nan::HandleScope scope;

  GetBaton* data = static_cast<GetBaton*>(req->data);

  v8::Local<v8::Value> argv[2];

  if (data->errorString[0]) {
    argv[0] = v8::Exception::Error(Nan::New<v8::String>(data->errorString).ToLocalChecked());
    argv[1] = Nan::Undefined();
  } else {
    v8::Local<v8::Object> results = Nan::New<v8::Object>();
    results->Set(Nan::New<v8::String>("cts").ToLocalChecked(), Nan::New<v8::Boolean>(data->cts));
    results->Set(Nan::New<v8::String>("dsr").ToLocalChecked(), Nan::New<v8::Boolean>(data->dsr));
    results->Set(Nan::New<v8::String>("dcd").ToLocalChecked(), Nan::New<v8::Boolean>(data->dcd));

    argv[0] = Nan::Null();
    argv[1] = results;
  }
  data->callback.Call(2, argv);

  delete data;
  delete req;
}

NAN_METHOD(Drain) {
  // file descriptor
  if (!info[0]->IsInt32()) {
    Nan::ThrowTypeError("First argument must be an int");
    return;
  }
  int fd = Nan::To<v8::Int32>(info[0]).ToLocalChecked()->Value();

  // callback
  if (!info[1]->IsFunction()) {
    Nan::ThrowTypeError("Second argument must be a function");
    return;
  }

  VoidBaton* baton = new VoidBaton();
  memset(baton, 0, sizeof(VoidBaton));
  baton->fd = fd;
  baton->callback.Reset(info[1].As<v8::Function>());

  uv_work_t* req = new uv_work_t();
  req->data = baton;
  uv_queue_work(uv_default_loop(), req, EIO_Drain, (uv_after_work_cb)EIO_AfterDrain);
}

void EIO_AfterDrain(uv_work_t* req) {
  Nan::HandleScope scope;

  VoidBaton* data = static_cast<VoidBaton*>(req->data);

  v8::Local<v8::Value> argv[1];

  if (data->errorString[0]) {
    argv[0] = v8::Exception::Error(Nan::New<v8::String>(data->errorString).ToLocalChecked());
  } else {
    argv[0] = Nan::Null();
  }
  data->callback.Call(1, argv);

  delete data;
  delete req;
}

SerialPortParity NAN_INLINE(ToParityEnum(const v8::Local<v8::String>& v8str)) {
  Nan::HandleScope scope;
  Nan::Utf8String str(v8str);
  size_t count = strlen(*str);
  SerialPortParity parity = SERIALPORT_PARITY_NONE;
  if (!strncasecmp(*str, "none", count)) {
    parity = SERIALPORT_PARITY_NONE;
  } else if (!strncasecmp(*str, "even", count)) {
    parity = SERIALPORT_PARITY_EVEN;
  } else if (!strncasecmp(*str, "mark", count)) {
    parity = SERIALPORT_PARITY_MARK;
  } else if (!strncasecmp(*str, "odd", count)) {
    parity = SERIALPORT_PARITY_ODD;
  } else if (!strncasecmp(*str, "space", count)) {
    parity = SERIALPORT_PARITY_SPACE;
  }
  return parity;
}

SerialPortStopBits NAN_INLINE(ToStopBitEnum(double stopBits)) {
  if (stopBits > 1.4 && stopBits < 1.6) {
    return SERIALPORT_STOPBITS_ONE_FIVE;
  }
  if (stopBits == 2) {
    return SERIALPORT_STOPBITS_TWO;
  }
  return SERIALPORT_STOPBITS_ONE;
}

extern "C" {
  void init(v8::Handle<v8::Object> target) {
    Nan::HandleScope scope;
    Nan::SetMethod(target, "set", Set);
    Nan::SetMethod(target, "get", Get);
    Nan::SetMethod(target, "open", Open);
    Nan::SetMethod(target, "update", Update);
    Nan::SetMethod(target, "write", Write);
#ifdef WIN32
    Nan::SetMethod(target, "read", Read);
#endif
    Nan::SetMethod(target, "close", Close);
    Nan::SetMethod(target, "list", List);
    Nan::SetMethod(target, "flush", Flush);
    Nan::SetMethod(target, "drain", Drain);

#ifndef WIN32
    ReadPoller::Init(target);
#endif
  }
}

NODE_MODULE(serialport, init);
