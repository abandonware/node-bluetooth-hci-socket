#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <node_buffer.h>
#include <nan.h>

#include "BluetoothHciSocket.h"

#define BTPROTO_L2CAP 0
#define BTPROTO_HCI   1

#define SOL_HCI       0
#define HCI_FILTER    2

#define HCIGETDEVLIST _IOR('H', 210, int)
#define HCIGETDEVINFO _IOR('H', 211, int)

#define HCI_CHANNEL_RAW     0
#define HCI_CHANNEL_USER    1
#define HCI_CHANNEL_CONTROL 3

#define HCI_DEV_NONE  0xffff

#define HCI_MAX_DEV 16

#define ATT_CID 4

enum {
  HCI_UP,
  HCI_INIT,
  HCI_RUNNING,

  HCI_PSCAN,
  HCI_ISCAN,
  HCI_AUTH,
  HCI_ENCRYPT,
  HCI_INQUIRY,

  HCI_RAW,
};

struct sockaddr_hci {
  sa_family_t     hci_family;
  unsigned short  hci_dev;
  unsigned short  hci_channel;
};

struct hci_dev_req {
  uint16_t dev_id;
  uint32_t dev_opt;
};

struct hci_dev_list_req {
  uint16_t dev_num;
  struct hci_dev_req dev_req[0];
};

struct hci_dev_info {
  uint16_t dev_id;
  char     name[8];

  bdaddr_t bdaddr;

  uint32_t flags;
  uint8_t  type;

  uint8_t  features[8];

  uint32_t pkt_type;
  uint32_t link_policy;
  uint32_t link_mode;

  uint16_t acl_mtu;
  uint16_t acl_pkts;
  uint16_t sco_mtu;
  uint16_t sco_pkts;

  // hci_dev_stats
  uint32_t err_rx;
  uint32_t err_tx;
  uint32_t cmd_tx;
  uint32_t evt_rx;
  uint32_t acl_tx;
  uint32_t acl_rx;
  uint32_t sco_tx;
  uint32_t sco_rx;
  uint32_t byte_rx;
  uint32_t byte_tx;
};


using namespace v8;

Nan::Persistent<FunctionTemplate> BluetoothHciSocket::constructor_template;

NAN_MODULE_INIT(BluetoothHciSocket::Init) {
  Nan::HandleScope scope;

  Local<FunctionTemplate> tmpl = Nan::New<FunctionTemplate>(New);
  constructor_template.Reset(tmpl);

  tmpl->InstanceTemplate()->SetInternalFieldCount(1);
  tmpl->SetClassName(Nan::New("BluetoothHciSocket").ToLocalChecked());

  Nan::SetPrototypeMethod(tmpl, "start", Start);
  Nan::SetPrototypeMethod(tmpl, "bindRaw", BindRaw);
  Nan::SetPrototypeMethod(tmpl, "bindUser", BindUser);
  Nan::SetPrototypeMethod(tmpl, "bindControl", BindControl);
  Nan::SetPrototypeMethod(tmpl, "isDevUp", IsDevUp);
  Nan::SetPrototypeMethod(tmpl, "getDeviceList", GetDeviceList);
  Nan::SetPrototypeMethod(tmpl, "setFilter", SetFilter);
  Nan::SetPrototypeMethod(tmpl, "stop", Stop);
  Nan::SetPrototypeMethod(tmpl, "write", Write);
  Nan::SetPrototypeMethod(tmpl, "cleanup", Cleanup);

  Nan::Set(target, Nan::New("BluetoothHciSocket").ToLocalChecked(), Nan::GetFunction(tmpl).ToLocalChecked());
}

BluetoothHciSocket::BluetoothHciSocket() :
  node::ObjectWrap(),
  _mode(0),
  _socket(-1),
  _devId(0),
  _pollHandle(),
  _address(),
  _addressType(0)
  {

  int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
  if (fd == -1) {
    Nan::ThrowError(Nan::ErrnoException(errno, "socket"));
    return;
  }
  this->_socket = fd;

  if (uv_poll_init(uv_default_loop(), &this->_pollHandle, this->_socket) < 0) {
    Nan::ThrowError("uv_poll_init failed");
    return;
  }

  this->_pollHandle.data = this;
}

BluetoothHciSocket::~BluetoothHciSocket() {
  uv_close((uv_handle_t*)&this->_pollHandle, (uv_close_cb)BluetoothHciSocket::PollCloseCallback);

  close(this->_socket);
}

void BluetoothHciL2Socket::connect(){
  _socket = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
  if(_socket < 0) return;

  if (bind(_socket, (struct sockaddr*)&l2_src, sizeof(l2_src)) < 0) {
    close(_socket);
    _socket = -1;
    return;
  }

  // the kernel needs to flush the socket before we continue
  while (::connect(_socket, (struct sockaddr *)&l2_dst, sizeof(l2_dst)) == -1 ) {
    if(errno == EINTR) {
      continue;
    }
    close(_socket);
    _socket = -1;
    break;
  }
}

void BluetoothHciL2Socket::disconnect(){
  if(this->_socket != -1) close(this->_socket);
  this->_socket = -1;
}

void BluetoothHciL2Socket::expires(uint64_t expires){
  _expires = expires;
}

uint64_t BluetoothHciL2Socket::expires() const {
  return _expires;
}

bool BluetoothHciL2Socket::connected() const {
  return this->_socket != -1;
}

BluetoothHciL2Socket::BluetoothHciL2Socket(BluetoothHciSocket* parent, unsigned char* srcaddr, char srcType, char* bdaddr, char bdaddrType, uint64_t expires): _parent(parent), _expires(expires), l2_src({}), l2_dst({}) {
    unsigned short l2cid;

#if __BYTE_ORDER == __LITTLE_ENDIAN
    l2cid = ATT_CID;
#elif __BYTE_ORDER == __BIG_ENDIAN
    l2cid = bswap_16(ATT_CID);
#else
    #error "Unknown byte order"
#endif
    
    memset(&l2_src, 0, sizeof(l2_src));
    l2_src.l2_family = AF_BLUETOOTH;
    l2_src.l2_cid = l2cid;
    memcpy(&l2_src.l2_bdaddr, srcaddr, sizeof(l2_src.l2_bdaddr));
    l2_src.l2_bdaddr_type = srcType;
      
    memset(&l2_dst, 0, sizeof(l2_dst));
    l2_dst.l2_family = AF_BLUETOOTH;
    memcpy(&l2_dst.l2_bdaddr, bdaddr, sizeof(l2_dst.l2_bdaddr));
    l2_dst.l2_cid = l2cid;
    l2_dst.l2_bdaddr_type = bdaddrType; // BDADDR_LE_PUBLIC (0x01), BDADDR_LE_RANDOM (0x02)

    connect();
}


BluetoothHciL2Socket::~BluetoothHciL2Socket(){
  if(this->_socket != -1) disconnect();
  if(_expires == 0){
    this->_parent->_l2sockets_connected.erase(l2_dst.l2_bdaddr);
  }
}

void BluetoothHciSocket::start() {
  if (uv_poll_start(&this->_pollHandle, UV_READABLE, BluetoothHciSocket::PollCallback) < 0) {
    Nan::ThrowError("uv_poll_start failed");
  }
}

int BluetoothHciSocket::bindRaw(int* devId) {
  struct sockaddr_hci a = {};
  struct hci_dev_info di = {};

  memset(&a, 0, sizeof(a));
  a.hci_family = AF_BLUETOOTH;
  a.hci_dev = this->devIdFor(devId, true);
  a.hci_channel = HCI_CHANNEL_RAW;

  this->_devId = a.hci_dev;
  this->_mode = HCI_CHANNEL_RAW;

  if (bind(this->_socket, (struct sockaddr *) &a, sizeof(a)) < 0) {
    Nan::ThrowError(Nan::ErrnoException(errno, "bind"));
    return -1;
  }

  // get the local address and address type
  memset(&di, 0x00, sizeof(di));
  di.dev_id = this->_devId;
  memset(_address, 0, sizeof(_address));
  _addressType = 0;

  if (ioctl(this->_socket, HCIGETDEVINFO, (void *)&di) > -1) {
    memcpy(_address, &di.bdaddr, sizeof(di.bdaddr));
    _addressType = di.type;

    if (_addressType == 3) {
      // 3 is a weird type, use 1 (public) instead
      _addressType = 1;
    }
  }

  return this->_devId;
}

int BluetoothHciSocket::bindUser(int* devId) {
  struct sockaddr_hci a = {};

  memset(&a, 0, sizeof(a));
  a.hci_family = AF_BLUETOOTH;
  a.hci_dev = this->devIdFor(devId, false);
  a.hci_channel = HCI_CHANNEL_USER;

  this->_devId = a.hci_dev;
  this->_mode = HCI_CHANNEL_USER;

  if (bind(this->_socket, (struct sockaddr *) &a, sizeof(a)) < 0) {
    Nan::ThrowError(Nan::ErrnoException(errno, "bind"));
    return -1;
  }

  return this->_devId;
}

void BluetoothHciSocket::bindControl() {
  struct sockaddr_hci a = {};

  memset(&a, 0, sizeof(a));
  a.hci_family = AF_BLUETOOTH;
  a.hci_dev = HCI_DEV_NONE;
  a.hci_channel = HCI_CHANNEL_CONTROL;

  this->_mode = HCI_CHANNEL_CONTROL;

  if (bind(this->_socket, (struct sockaddr *) &a, sizeof(a)) < 0) {
    Nan::ThrowError(Nan::ErrnoException(errno, "bind"));
    return;
  }
}

bool BluetoothHciSocket::isDevUp() {
  struct hci_dev_info di = {};
  bool isUp = false;

  memset(&di, 0x00, sizeof(di));
  di.dev_id = this->_devId;

  if (ioctl(this->_socket, HCIGETDEVINFO, (void *)&di) > -1) {
    isUp = (di.flags & (1 << HCI_UP)) != 0;
  }

  return isUp;
}

void BluetoothHciSocket::setFilter(char* data, int length) {
  if (setsockopt(this->_socket, SOL_HCI, HCI_FILTER, data, length) < 0) {
    this->emitErrnoError("setsockopt");
  }
}

void BluetoothHciSocket::poll() {
  Nan::HandleScope scope;

  int length = 0;
  char data[1024];

  length = read(this->_socket, data, sizeof(data));

  if (length > 0) {
    if (this->_mode == HCI_CHANNEL_RAW) {
      // TODO: This does not check for the retval of this function – should it?
      this->kernelDisconnectWorkArounds(length, data);
    }

    Local<Value> argv[2] = {
      Nan::New("data").ToLocalChecked(),
      Nan::CopyBuffer(data, length).ToLocalChecked()
    };

    Nan::AsyncResource res("BluetoothHciSocket::poll");
    res.runInAsyncScope(
      Nan::New<Object>(this->This),
      Nan::New("emit").ToLocalChecked(),
      2,
      argv
    ).FromMaybe(v8::Local<v8::Value>());
  }
}

void BluetoothHciSocket::stop() {
  uv_poll_stop(&this->_pollHandle);
}

void BluetoothHciSocket::write_(char* data, int length) {
  if (this->_mode == HCI_CHANNEL_RAW && this->kernelConnectWorkArounds(data, length)) {
    return;
  }

  if (write(this->_socket, data, length) < 0) {
    this->emitErrnoError("write");
  }
}

void BluetoothHciSocket::emitErrnoError(const char *syscall) {
  v8::Local<v8::Value> error = Nan::ErrnoException(errno, syscall, strerror(errno));

  Local<Value> argv[2] = {
    Nan::New("error").ToLocalChecked(),
    error
  };
  Nan::AsyncResource res("BluetoothHciSocket::emitErrnoError");
  res.runInAsyncScope(
    Nan::New<Object>(this->This),
    Nan::New("emit").ToLocalChecked(),
    2,
    argv
  ).FromMaybe(v8::Local<v8::Value>());
}

int BluetoothHciSocket::devIdFor(const int* pDevId, bool isUp) {
  int devId = 0; // default

  if (pDevId == nullptr) {
    struct hci_dev_list_req *dl;
    struct hci_dev_req *dr;

    dl = (hci_dev_list_req*)calloc(HCI_MAX_DEV * sizeof(*dr) + sizeof(*dl), 1);
    dr = dl->dev_req;

    dl->dev_num = HCI_MAX_DEV;

    if (ioctl(this->_socket, HCIGETDEVLIST, dl) > -1) {
      for (int i = 0; i < dl->dev_num; i++, dr++) {
        bool devUp = dr->dev_opt & (1 << HCI_UP);
        bool match = (isUp == devUp);

        if (match) {
          // choose the first device that is match
          // later on, it would be good to also HCIGETDEVINFO and check the HCI_RAW flag
          devId = dr->dev_id;
          break;
        }
      }
    }

    free(dl);
  } else {
    devId = *pDevId;
  }

  return devId;
}

int BluetoothHciSocket::kernelDisconnectWorkArounds(int length, char* data) {
  // HCI Event - LE Meta Event - LE Connection Complete => manually create L2CAP socket to force kernel to book keep
  // this socket will be closed immediately.

  // The if statement:
  // data[0] = LE Meta Event (HCI_EVENT_PKT)
  // data[1] = HCI_EV_LE_META
  // data[2] = plen (0x13)
  // data[3] = HCI_EV_LE_CONN_COMPLETE (0x01)
  // data[4] = Status (0x00 = Success)
  // data[5,6] = handle (little endian)
  // data[7] = role (0x00 = Master)
  // data[9,]  = device bt address
  if (length == 22 && data[0] == 0x04 && data[1] == 0x3e && data[2] == 0x13 && data[3] == 0x01 && data[4] == 0x00) { //  && data[7] == 0x01
    unsigned short handle = *((unsigned short*)(&data[5]));
    //printf("HCI_EV_LE_CONN_COMPLETE for handle %d\n", handle);

    std::shared_ptr<BluetoothHciL2Socket> l2socket_ptr;

    auto it = _l2sockets_connected.find(*(bdaddr_t*)&data[9]);
    if(it != _l2sockets_connected.end()){
      l2socket_ptr = it->second.lock();
    } else {
      auto it2 = _l2sockets_connecting.find(*(bdaddr_t*)&data[9]);

      if(it2 != _l2sockets_connecting.end()){
        //successful connection (we have a handle for the socket!)
        l2socket_ptr = it2->second;
        l2socket_ptr->expires(0);
        _l2sockets_connecting.erase(it2);
      } else {
        l2socket_ptr = std::make_shared<BluetoothHciL2Socket>(this, _address, _addressType, &data[9], data[8] + 1, 0);
        if(!l2socket_ptr->connected()){
          //printf("%02x:%02x:%02x:%02x:%02x:%02x handle %d was not connected\n", data[9], data[10], data[11], data[12], data[13], data[14],  handle);
          return 0;
        }
        this->_l2sockets_connected[*(bdaddr_t*)&data[9]] = l2socket_ptr;
      }
    }

      
    if(!l2socket_ptr->connected()){
      //printf("%02x:%02x:%02x:%02x:%02x:%02x handle %d was not connected (2)\n", data[9], data[10], data[11], data[12], data[13], data[14], handle);
      return 0;
    }

    
    handle = handle % 256;
    this->_l2sockets_handles[handle] = l2socket_ptr;
  } else if (length == 7 && data[0] == 0x04 && data[1] == 0x05 && data[2] == 0x04 && data[3] == 0x00) {
    
    // HCI Event - Disconn Complete =======================> close socket from above
    unsigned short handle = *((unsigned short*)(&data[4]));
    //printf("Disconn Complete for handle %d (%d)\n", handle, this->_l2sockets_handles.count(handle));
    handle = handle % 256;
    this->_l2sockets_handles.erase(handle);
  }

  return 0;
}

void BluetoothHciSocket::setConnectionParameters(
    unsigned short connMinInterval,
    unsigned short connMaxInterval,
    unsigned short connLatency,
    unsigned short supervisionTimeout
){
    char command[128];

    // override the HCI devices connection parameters using debugfs
    sprintf(command, "echo %u > /sys/kernel/debug/bluetooth/hci%d/conn_min_interval", connMinInterval, this->_devId);
    system(command);
    sprintf(command, "echo %u > /sys/kernel/debug/bluetooth/hci%d/conn_max_interval", connMaxInterval, this->_devId);
    system(command);
    sprintf(command, "echo %u > /sys/kernel/debug/bluetooth/hci%d/conn_latency", connLatency, this->_devId);
    system(command);
    sprintf(command, "echo %u > /sys/kernel/debug/bluetooth/hci%d/supervision_timeout", supervisionTimeout, this->_devId);
    system(command);
}


bool BluetoothHciSocket::kernelConnectWorkArounds(char* data, int length)
{
  // if statement:
  // data[0]: HCI_COMMAND_PKT
  // data[1,2]: HCI_OP_LE_CREATE_CONN (0x200d)
  // data[3]: plen
  // data[10 ...] bdaddr

  if (length == 29 && data[0] == 0x01 && data[1] == 0x0d && data[2] == 0x20 && data[3] == 0x19) {
    unsigned short connMinInterval;
    unsigned short connMaxInterval;
    unsigned short connLatency;
    unsigned short supervisionTimeout;

    //printf("HCI_OP_LE_CREATE_CONN %02x:%02x:%02x:%02x:%02x:%02x\n", data[10], data[11], data[12], data[13], data[14], data[15]);

    // extract the connection parameter
    connMinInterval = (data[18] << 8) | data[17];
    connMaxInterval = (data[20] << 8) | data[19];
    connLatency = (data[22] << 8) | data[21];
    supervisionTimeout = (data[24] << 8) | data[23];

    this->setConnectionParameters(connMinInterval, connMaxInterval, connLatency, supervisionTimeout);

    std::shared_ptr<BluetoothHciL2Socket> l2socket_ptr;
    if(this->_l2sockets_connected.find(*(bdaddr_t*)&data[10]) != this->_l2sockets_connected.end()){
      // we are refreshing the connection (which was connected)
      l2socket_ptr = this->_l2sockets_connected[*(bdaddr_t*)&data[10]].lock();
      l2socket_ptr->disconnect();
      l2socket_ptr->connect();
      // no expiration as we will continue to be "connected" on the other handle which must exist
    } else if(this->_l2sockets_connecting.find(*(bdaddr_t*)&data[10]) != this->_l2sockets_connecting.end()){
      // we were connecting but now we connect again
      l2socket_ptr = this->_l2sockets_connecting[*(bdaddr_t*)&data[10]];
      l2socket_ptr->disconnect();
      l2socket_ptr->connect();
      l2socket_ptr->expires(uv_hrtime() + L2_CONNECT_TIMEOUT);
    } else{    
      // 60000000000  = 1 minute
      l2socket_ptr = std::make_shared<BluetoothHciL2Socket>(this, _address, _addressType, &data[10], data[9] + 1, uv_hrtime() + L2_CONNECT_TIMEOUT);
      if(!l2socket_ptr->connected()){
        return false;
      }
      this->_l2sockets_connecting[*(bdaddr_t*)&data[10]] = l2socket_ptr;
    }

    // returns true to skip sending the kernel this commoand
    // the command will instead be sent by the connect() operation
    return true;
  }

  return false;
}

NAN_METHOD(BluetoothHciSocket::Cleanup) {
  BluetoothHciSocket* p = node::ObjectWrap::Unwrap<BluetoothHciSocket>(info.This());
  auto now = uv_hrtime();

  for (auto it = p->_l2sockets_connecting.cbegin(); it != p->_l2sockets_connecting.cend() /* not hoisted */; /* no increment */)
  {
    if (now < it->second->expires())
    {
      p->_l2sockets_connecting.erase(it++);    // or "it = m.erase(it)" since C++11
    }
    else
    {
      ++it;
    }
  }
}

NAN_METHOD(BluetoothHciSocket::New) {
  Nan::HandleScope scope;

  BluetoothHciSocket* p = new BluetoothHciSocket();
  p->Wrap(info.This());
  p->This.Reset(info.This());
  info.GetReturnValue().Set(info.This());
}

NAN_METHOD(BluetoothHciSocket::Start) {
  Nan::HandleScope scope;

  BluetoothHciSocket* p = node::ObjectWrap::Unwrap<BluetoothHciSocket>(info.This());

  p->start();

  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(BluetoothHciSocket::BindRaw) {
  Nan::HandleScope scope;

  BluetoothHciSocket* p = node::ObjectWrap::Unwrap<BluetoothHciSocket>(info.This());

  int devId = 0;
  int* pDevId = nullptr;

  if (info.Length() > 0) {
    Local<Value> arg0 = info[0];
    if (arg0->IsInt32() || arg0->IsUint32()) {
      devId = Nan::To<int32_t>(arg0).FromJust();

      pDevId = &devId;
    }
  }

  devId = p->bindRaw(pDevId);

  info.GetReturnValue().Set(devId);
}

NAN_METHOD(BluetoothHciSocket::BindUser) {
  Nan::HandleScope scope;

  BluetoothHciSocket* p = node::ObjectWrap::Unwrap<BluetoothHciSocket>(info.This());

  int devId = 0;
  int* pDevId = nullptr;

  if (info.Length() > 0) {
    Local<Value> arg0 = info[0];
    if (arg0->IsInt32() || arg0->IsUint32()) {
      devId = Nan::To<int32_t>(arg0).FromJust();

      pDevId = &devId;
    }
  }

  devId = p->bindUser(pDevId);

  info.GetReturnValue().Set(devId);
}

NAN_METHOD(BluetoothHciSocket::BindControl) {
  Nan::HandleScope scope;

  BluetoothHciSocket* p = node::ObjectWrap::Unwrap<BluetoothHciSocket>(info.This());

  p->bindControl();

  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(BluetoothHciSocket::IsDevUp) {
  Nan::HandleScope scope;

  BluetoothHciSocket* p = node::ObjectWrap::Unwrap<BluetoothHciSocket>(info.This());

  bool isDevUp = p->isDevUp();

  info.GetReturnValue().Set(isDevUp);
}

NAN_METHOD(BluetoothHciSocket::GetDeviceList) {
  Nan::HandleScope scope;

  BluetoothHciSocket* p = node::ObjectWrap::Unwrap<BluetoothHciSocket>(info.This());

  struct hci_dev_list_req *dl;
  struct hci_dev_req *dr;

  dl = (hci_dev_list_req*)calloc(HCI_MAX_DEV * sizeof(*dr) + sizeof(*dl), 1);
  dr = dl->dev_req;

  dl->dev_num = HCI_MAX_DEV;

  Local<Array> deviceList = Nan::New<v8::Array>();

  if (ioctl(p->_socket, HCIGETDEVLIST, dl) > -1) {
    int di = 0;
    for (int i = 0; i < dl->dev_num; i++, dr++) {
      uint16_t devId = dr->dev_id;
      bool devUp = dr->dev_opt & (1 << HCI_UP);
      // TODO: smells like there's a bug here (but dr isn't read so...)
      if (dr != nullptr) {
        v8::Local<v8::Object> obj = Nan::New<v8::Object>();
        Nan::Set(obj, Nan::New("devId").ToLocalChecked(), Nan::New<Number>(devId));
        Nan::Set(obj, Nan::New("devUp").ToLocalChecked(), Nan::New<Boolean>(devUp));
        Nan::Set(obj, Nan::New("idVendor").ToLocalChecked(), Nan::Null());
        Nan::Set(obj, Nan::New("idProduct").ToLocalChecked(), Nan::Null());
        Nan::Set(obj, Nan::New("busNumber").ToLocalChecked(), Nan::Null());
        Nan::Set(obj, Nan::New("deviceAddress").ToLocalChecked(), Nan::Null());
        Nan::Set(deviceList, di++, obj);
      }
    }
  }

  free(dl);

  info.GetReturnValue().Set(deviceList);
}

NAN_METHOD(BluetoothHciSocket::SetFilter) {
  Nan::HandleScope scope;

  BluetoothHciSocket* p = node::ObjectWrap::Unwrap<BluetoothHciSocket>(info.This());

  if (info.Length() > 0) {
    Local<Value> arg0 = info[0];
    if (arg0->IsObject()) {
      p->setFilter(node::Buffer::Data(arg0), node::Buffer::Length(arg0));
    }
  }

  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(BluetoothHciSocket::Stop) {
  Nan::HandleScope scope;

  BluetoothHciSocket* p = node::ObjectWrap::Unwrap<BluetoothHciSocket>(info.This());

  p->stop();

  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(BluetoothHciSocket::Write) {
  Nan::HandleScope scope;
  BluetoothHciSocket* p = node::ObjectWrap::Unwrap<BluetoothHciSocket>(info.This());

  if (info.Length() > 0) {
    Local<Value> arg0 = info[0];
    if (arg0->IsObject()) {

      p->write_(node::Buffer::Data(arg0), node::Buffer::Length(arg0));
    }
  }

  info.GetReturnValue().SetUndefined();
}


void BluetoothHciSocket::PollCloseCallback(uv_poll_t* handle) {
  delete handle;
}

void BluetoothHciSocket::PollCallback(uv_poll_t* handle, int status, int events) {
  BluetoothHciSocket *p = (BluetoothHciSocket*)handle->data;

  p->poll();
}

NODE_MODULE(binding, BluetoothHciSocket::Init);
