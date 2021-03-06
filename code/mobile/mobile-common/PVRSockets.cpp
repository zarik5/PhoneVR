#include "PVRSockets.h"

#include <queue>
#include "PVRSocketUtils.h"
#include "Utils/ThreadUtils.h"

//using namespace PVR;
using namespace std;
using namespace asio;
using namespace asio::ip;
using namespace std::chrono;

namespace {
    TCPTalker *talker = nullptr;
    io_service *videoSvc = nullptr;
    std::thread *strThr = nullptr;
    io_service *annSvc = nullptr;
    string pcIP;
    mutex delMtx;

    bool announcing = false;

    TimeBomb headerBomb(seconds(5), []{ pvrState = PVR_STATE_SHUTDOWN; }); // todo: error handling instead of shutdown

    //           pts       buf
    queue<pair<int64_t, vector<float>>> quatQueue;

    queue<EmptyVidBuf> emptyVBufs;
    queue<FilledVidBuf> filledVBufs;
}

vector<float> DequeueQuatAtPts(int64_t pts) {
    vector<float> quat;
    while (quatQueue.size() > 0 && quatQueue.front().first <= pts) {
        quat = quatQueue.front().second;
        quatQueue.pop();
    }
    return quat;
}

void SendAdditionalData(vector<uint16_t> maxSize, vector<float> fov, float ipd) {
    if(talker){
        vector<uint8_t> v(2 * 2 + 4 * 4 + 4);
        memcpy(&v[0], &maxSize[0], 2 * 2);
        memcpy(&v[2 * 2], &fov[0], 4 * 4);
        memcpy(&v[2 * 2 + 4 * 4], &ipd, 4);
        talker->send(PVR_MSG::ADDITIONAL_DATA, v);
        headerBomb.ignite(false);
    }
}

void PVRStartAnnouncer(const char *ip, uint16_t port, void(*segueCb)(), void(*headerCb)(uint8_t *, size_t), void(*unwindSegue)()) {
    pcIP = ip; //ip will become invalid afterwards, so I capture a string copy
    std::thread([=] {
        if (talker){
            talker->send(PVR_MSG::DISCONNECT);
            delete talker;
        }
        talker = new TCPTalker(port, [=](PVR_MSG msgType, vector<uint8_t> data) {
            if (msgType == PVR_MSG::PAIR_ACCEPT) {
                announcing = false;
                pcIP = talker->getIP();
                segueCb();
            }
            else if (msgType == PVR_MSG::HEADER_NALS) {
                headerCb(&data[0], data.size());
                headerBomb.defuse();
            }
            else if (msgType == PVR_MSG::DISCONNECT) {
                unwindSegue();
            }
        }, [](std::error_code err) {
            PVR_DB("TCP error: " + err.message());
        }, true, pcIP);

        io_service svc;
        udp::socket skt(svc);
        skt.open(udp::v4());
        udp::endpoint remEP;
        if (!pcIP.empty())
            remEP = {address::from_string(pcIP), port};
        else
        {   // broadcast if ip is unset
            skt.set_option(socket_base::broadcast(true));
            remEP = {address_v4::broadcast(), port};
        }

        uint8_t buf[8] = {'p', 'v', 'r', PVR_MSG::PAIR_HMD};
        auto vers = PVR_CLIENT_VERSION;
        memcpy(&buf[4], &vers, 4);

        announcing = true;

        while (announcing) {
            skt.send_to(buffer(buf, 8), remEP);
            auto ec = asio::error_code();
            size_t pktSz = 0;
            //TimeBomb bomb(milliseconds());


            //usleep(1000000); // 1s
        }
    }).detach();
}

void PVRStopAnnouncer() {
    announcing = false;
}

void PVRStartSendSensorData(uint16_t port, bool(*getSensorData)(float *, float *)) {
    std::thread([=] {
        io_service svc;
        udp::socket skt(svc);
        udp::endpoint ep(address::from_string(pcIP), port);
        skt.open(udp::v4());

        RefWhistle ref(microseconds(8333)); // this scans loops of exactly 120 fps

        uint8_t buf[36];
        auto orQuat = reinterpret_cast<float *>(&buf[0]);
        auto acc = reinterpret_cast<float *>(&buf[4 * 4]);
        auto tm = reinterpret_cast<long long *>(&buf[4 * 4 + 3 * 4]);
        while (pvrState != PVR_STATE_SHUTDOWN)
        {
            if (getSensorData(orQuat, acc))
            {
                *tm = Clk::now().time_since_epoch().count();
                skt.send_to(buffer(buf, 36), ep);
            }
            ref.wait();
        }
    }).detach();
}

bool PVRIsVidBufNeeded(){
    return emptyVBufs.size() < 3;
}

void PVREnqueueVideoBuf(EmptyVidBuf eBuf){
    emptyVBufs.push(eBuf);
}

FilledVidBuf PVRPopVideoBuf(){
    if (!filledVBufs.empty())
    {
        auto fbuf = filledVBufs.front();
        filledVBufs.pop();
        return fbuf;
    }
    return {-1, 0, 0}; //idx == -1 -> no buffers available
}

void PVRStartReceiveStreams(uint16_t port) {
    while (pvrState == PVR_STATE_SHUTDOWN)
        usleep(10000);
    strThr = new std::thread([=] {
        io_service svc;
        videoSvc = &svc;
        //udp::socket skt(svc, {udp::v4(), port});
        tcp::socket skt(svc);
        asio::error_code ec = error::fault;
        while (ec.value() != 0 && pvrState != PVR_STATE_SHUTDOWN)
            skt.connect({address::from_string(pcIP), port}, ec);

        function<void(const asio::error_code &, size_t)> handler = [&](const asio::error_code &err, size_t) { ec = err; };

        uint8_t extraBuf[28];
        auto pts = reinterpret_cast<int64_t *>(extraBuf);              // these values are automatically updated
        auto quatBuf = reinterpret_cast<float *>(extraBuf + 8);        // when extraBuf is updated
        auto pktSz = reinterpret_cast<uint32_t *>(extraBuf + 8 + 16);

        // reinit queues
        quatQueue = queue<pair<int64_t, vector<float>>>();
        emptyVBufs = queue<EmptyVidBuf>();
        filledVBufs = queue<FilledVidBuf>();

        while(pvrState != PVR_STATE_SHUTDOWN)
        {
            async_read(skt, buffer(extraBuf, 28), handler);
            svc.run();
            svc.reset();

            if (ec.value() == 0)
            {
                quatQueue.push({*pts, vector<float>(quatBuf, quatBuf + 4)});

                while (emptyVBufs.empty() && pvrState != PVR_STATE_SHUTDOWN)
                    usleep(2000); //1ms

                if (!emptyVBufs.empty())
                {
                    auto eBuf = emptyVBufs.front();
                    async_read(skt, buffer(eBuf.buf, *pktSz), handler);
                    svc.run();
                    svc.reset();
                    if (ec.value() == 0)
                    {
                        filledVBufs.push({eBuf.idx, *pktSz, (uint64_t)*pts});
                        emptyVBufs.pop();
                    }
                    else
                        break;
                }
            }
            else
                break;
        }
        delMtx.lock();
        videoSvc = nullptr;
        delMtx.unlock();
    });
}

void PVRStopStreams() {
    // talker sends disconnects at segue
    delMtx.lock();
    if (videoSvc)
        videoSvc->stop(); //todo: use mutex
    delMtx.unlock();

    if (strThr) {
        strThr->join();
        delete strThr;
        strThr = nullptr;
    }
}