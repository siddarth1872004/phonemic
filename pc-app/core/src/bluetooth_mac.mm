// macOS: publish the PhoneMic RFCOMM service with IOBluetooth (the system's own
// Bluetooth stack). IOBluetooth is callback-driven: the server thread runs a run
// loop that receives channel-open and data callbacks; MacStream bridges them to
// the blocking ByteStream interface the shared code uses. Built with ARC.
#include "phonemic/bluetooth.hpp"

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

#include <phonemic/wire.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>

namespace phonemic {
namespace {
struct MacChan {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<uint8_t> in;
    bool closed = false;
    IOBluetoothRFCOMMChannel* ch = nil;
    id delegate = nil;  // the channel does not retain its delegate
    CFRunLoopRef loop = nullptr;
    ~MacChan() {
        if (loop) CFRelease(loop);
    }
};
}  // namespace
}  // namespace phonemic

using phonemic::MacChan;

@interface PMChannelDelegate : NSObject <IOBluetoothRFCOMMChannelDelegate> {
@public
    std::shared_ptr<MacChan> chan;
}
@end

@implementation PMChannelDelegate
- (void)rfcommChannelData:(IOBluetoothRFCOMMChannel*)channel data:(void*)data length:(size_t)length {
    const auto* p = static_cast<const uint8_t*>(data);
    std::lock_guard<std::mutex> lk(chan->mu);
    chan->in.insert(chan->in.end(), p, p + length);
    chan->cv.notify_all();
}
- (void)rfcommChannelClosed:(IOBluetoothRFCOMMChannel*)channel {
    std::lock_guard<std::mutex> lk(chan->mu);
    chan->closed = true;
    chan->cv.notify_all();
}
@end

namespace phonemic {
namespace {

// Run `fn` on the server thread's run loop (IOBluetooth objects live there).
void on_loop(const std::shared_ptr<MacChan>& c, void (^fn)(void)) {
    CFRunLoopPerformBlock(c->loop, kCFRunLoopCommonModes, fn);
    CFRunLoopWakeUp(c->loop);
}

class MacStream : public ByteStream {
public:
    explicit MacStream(std::shared_ptr<MacChan> c) : c_(std::move(c)) {}
    ~MacStream() override {
        close();
        auto c = c_;
        on_loop(c, ^{
            [c->ch setDelegate:nil];
            c->delegate = nil;  // breaks the delegate <-> MacChan cycle
            c->ch = nil;
        });
    }

    int read(uint8_t* buf, size_t cap, int timeout_ms) override {
        std::unique_lock<std::mutex> lk(c_->mu);
        c_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return !c_->in.empty() || c_->closed; });
        if (c_->in.empty()) return c_->closed ? -1 : 0;
        const size_t n = std::min(cap, c_->in.size());
        std::copy(c_->in.begin(), c_->in.begin() + static_cast<std::ptrdiff_t>(n), buf);
        c_->in.erase(c_->in.begin(), c_->in.begin() + static_cast<std::ptrdiff_t>(n));
        return static_cast<int>(n);
    }

    bool write(const uint8_t* p, size_t n) override {
        {
            std::lock_guard<std::mutex> lk(c_->mu);
            if (c_->closed) return false;
        }
        NSData* d = [NSData dataWithBytes:p length:n];
        auto c = c_;
        on_loop(c, ^{
            [c->ch writeSync:const_cast<void*>(d.bytes) length:static_cast<UInt16>(d.length)];
        });
        return true;
    }

    void close() override {
        {
            std::lock_guard<std::mutex> lk(c_->mu);
            c_->closed = true;
            c_->cv.notify_all();
        }
        auto c = c_;
        on_loop(c, ^{
            [c->ch closeChannel];
        });
    }

private:
    std::shared_ptr<MacChan> c_;
};

bool service_uuid_bytes(uint8_t out[16]) {
    size_t n = 0;
    for (const char* p = PM_SERVICE_UUID; *p && n < 32; ++p) {
        if (*p == '-') continue;
        const char pair[3] = {p[0], p[1], 0};
        if (!p[1]) return false;
        out[n / 2] = static_cast<uint8_t>(std::strtoul(pair, nullptr, 16));
        n += 2;
        ++p;
    }
    return n == 32;
}

}  // namespace
}  // namespace phonemic

@interface PMListener : NSObject {
@public
    phonemic::BluetoothServer* server;
    CFRunLoopRef loop;
}
- (void)newChannel:(IOBluetoothUserNotification*)note channel:(IOBluetoothRFCOMMChannel*)channel;
@end

@implementation PMListener
- (void)newChannel:(IOBluetoothUserNotification*)note channel:(IOBluetoothRFCOMMChannel*)channel {
    auto c = std::make_shared<MacChan>();
    c->ch = channel;
    c->loop = static_cast<CFRunLoopRef>(CFRetain(loop));
    PMChannelDelegate* d = [PMChannelDelegate new];
    d->chan = c;
    c->delegate = d;
    [channel setDelegate:d];
    NSString* name = [[channel getDevice] nameOrAddress];
    server->accept(std::make_unique<phonemic::MacStream>(c), name ? std::string(name.UTF8String) : std::string());
}
@end

namespace phonemic {

void BluetoothServer::platform_loop() {
    @autoreleasepool {
        PMListener* listener = [PMListener new];
        listener->server = this;
        listener->loop = CFRunLoopGetCurrent();
        IOBluetoothSDPServiceRecord* record = nil;
        IOBluetoothUserNotification* note = nil;

        while (!stop_.load()) {
            @autoreleasepool {
                IOBluetoothHostController* hc = [IOBluetoothHostController defaultController];
                if (!hc || !hc.addressAsString) {
                    set_state(State::Unavailable, "no Bluetooth adapter found");
                } else if (hc.powerState != kBluetoothHCIPowerStateON) {
                    set_state(State::PoweredOff, "turn Bluetooth on in System Settings");
                } else {
                    if (NSString* n = hc.nameAsString) set_adapter_name(n.UTF8String);
                    if (!record) {
                        uint8_t uuid[16];
                        service_uuid_bytes(uuid);
                        NSDictionary* channel = @{
                            @"DataElementType" : @1,  // unsigned int; the system assigns the real channel
                            @"DataElementSize" : @1,
                            @"DataElementValue" : @1,
                        };
                        NSDictionary* dict = @{
                            @"0001 - ServiceClassIDList" : @[ [IOBluetoothSDPUUID uuidWithBytes:uuid length:16] ],
                            @"0004 - ProtocolDescriptorList" : @[
                                @[ [IOBluetoothSDPUUID uuid16:kBluetoothSDPUUID16L2CAP] ],
                                @[ [IOBluetoothSDPUUID uuid16:kBluetoothSDPUUID16RFCOMM], channel ],
                            ],
                            @"0005 - BrowseGroupList*" :
                                @[ [IOBluetoothSDPUUID uuid16:kBluetoothSDPUUID16ServiceClassPublicBrowseGroup] ],
                            @"0100 - ServiceName*" : @(PM_SERVICE_NAME),
                        };
                        record = [IOBluetoothSDPServiceRecord publishedServiceRecordWithDictionary:dict];
                        BluetoothRFCOMMChannelID ch = 0;
                        if (record && [record getRFCOMMChannelID:&ch] == kIOReturnSuccess) {
                            note = [IOBluetoothRFCOMMChannel
                                registerForChannelOpenNotifications:listener
                                                           selector:@selector(newChannel:channel:)
                                                      withChannelID:ch
                                                          direction:kIOBluetoothUserNotificationChannelDirectionIncoming];
                        }
                        if (!note) {
                            [record removeServiceRecord];
                            record = nil;
                            set_state(State::Failed, "can't publish the PhoneMic service");
                        }
                    }
                    if (record) set_state(State::Ready);
                }
            }
            // Deliver IOBluetooth callbacks (and MacStream writes) for 3 s.
            for (int i = 0; i < 12 && !stop_.load(); ++i) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto* s : streams_) s->close();
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);  // run the queued closes
        [note unregister];
        [record removeServiceRecord];
    }
}

}  // namespace phonemic
