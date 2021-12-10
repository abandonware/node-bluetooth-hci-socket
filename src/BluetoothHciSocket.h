#ifndef ___BLUETOOTH_HCI_SOCKET_H___
#define ___BLUETOOTH_HCI_SOCKET_H___

#include <node.h>
#include <map>
#include <nan.h>
#include <memory>

// 1 minute in nanoseconds
#define L2_CONNECT_TIMEOUT 60000000000

typedef struct bdaddr_s {
  uint8_t b[6];

  bool operator<(const struct bdaddr_s& r) const {
    for(int i = 0; i < 6; i++) {
      if(b[i] >= r.b[i]) {
        return false;
      }
    }
    return true;
  }

} __attribute__((packed)) bdaddr_t;

struct sockaddr_l2 {
  sa_family_t    l2_family;
  unsigned short l2_psm;
  bdaddr_t       l2_bdaddr;
  unsigned short l2_cid;
  uint8_t        l2_bdaddr_type;
};

class BluetoothHciSocket;

class BluetoothHciL2Socket {
  public:
  BluetoothHciL2Socket(BluetoothHciSocket* parent, unsigned char*, char, char*, char, uint64_t expires);
  ~BluetoothHciL2Socket();
  void disconnect();
  void connect();
  void expires(uint64_t expires);
  uint64_t expires() const;
  bool connected() const;


  private:
  int _socket;
  BluetoothHciSocket* _parent;
  uint64_t _expires; // or 0 if connected
  struct sockaddr_l2 l2_src;
  struct sockaddr_l2 l2_dst;
};

class BluetoothHciSocket : public node::ObjectWrap {
  friend class BluetoothHciL2Socket;

public:
  static NAN_MODULE_INIT(Init);

  static NAN_METHOD(New);
  static NAN_METHOD(BindRaw);
  static NAN_METHOD(BindUser);
  static NAN_METHOD(BindControl);
  static NAN_METHOD(IsDevUp);
  static NAN_METHOD(GetDeviceList);
  static NAN_METHOD(SetFilter);
  static NAN_METHOD(Start);
  static NAN_METHOD(Stop);
  static NAN_METHOD(Write);
  static NAN_METHOD(Cleanup);

private:
  BluetoothHciSocket();
  ~BluetoothHciSocket();

  void start();
  int bindRaw(int* devId);
  int bindUser(int* devId);
  void bindControl();
  bool isDevUp();
  void setFilter(char* data, int length);
  void stop();

  void write_(char* data, int length);

  void poll();

  void emitErrnoError(const char *syscall);
  int devIdFor(const int* devId, bool isUp);
  int kernelDisconnectWorkArounds(int length, char* data);
  bool kernelConnectWorkArounds(char* data, int length);
  void setConnectionParameters(unsigned short connMinInterval, unsigned short connMaxInterval, unsigned short connLatency, unsigned short supervisionTimeout);

  static void PollCloseCallback(uv_poll_t* handle);
  static void PollCallback(uv_poll_t* handle, int status, int events);

private:
  Nan::Persistent<v8::Object> This;

  int _mode;
  int _socket;
  int _devId;
  uv_poll_t _pollHandle;
  uint8_t _address[6];
  uint8_t _addressType;
  std::map<bdaddr_t, std::weak_ptr<BluetoothHciL2Socket>> _l2sockets_connected;
  std::map<bdaddr_t, std::shared_ptr<BluetoothHciL2Socket>> _l2sockets_connecting;
  std::map<unsigned short, std::shared_ptr<BluetoothHciL2Socket>> _l2sockets_handles;

  static Nan::Persistent<v8::FunctionTemplate> constructor_template;
};

#endif
