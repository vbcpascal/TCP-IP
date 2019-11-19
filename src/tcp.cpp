#include "tcp.h"

#define SENDLST_POP(lck) \
  {                      \
    lck.lock();          \
    sendList.pop();      \
    lck.unlock();        \
  }

#define SENDLST_POP_NOTIFY(lck) \
  {                             \
    SENDLST_POP(lck);           \
    sendCv.notify_all();        \
  }

namespace Tcp {

TcpWorker::TcpWorker() : st(TcpState::CLOSED) {
  seq.snd_una = seq.snd_nxt = seq.snd_isn = Sequence::isnGen.getISN();
  syned.store(false);
  sender = std::thread([&]() { senderLoop(); });
  sender.detach();
}

TcpState TcpWorker::getSt() {
  auto s = st.load();
  return s;
}

void TcpWorker::setSt(TcpState newst) {
  LOG_INFO("Change State to: \033[33;1m%s\033[0m", stateToStr(newst).c_str());
  st.store(newst);
  criticalSt.store(newst);
  return;
}

TcpState TcpWorker::getCriticalSt() {
  auto s = criticalSt.load();
  return s;
}

void TcpWorker::setCriticalSt(TcpState newst) {
  LOG_INFO("Change Critical State to: \033[33;1m%s\033[0m",
           stateToStr(newst).c_str());
  criticalSt.store(newst);
  return;
}

ssize_t TcpWorker::send(const TcpItem& ti) {
  /*
   * Should be the UNIQUE entrance to send a packet. Similiar to senderLoop
   * mentioned next, we will compare the sequence number to send and rcv_nxt.
   * After that, if the abandonedSeq has the same seq, it failed.
   *
   * See also: senderLoop
   */
  // Printer::printTcpItem(ti);

  auto currSeq = ti.ts.hdr.th_seq;
  {  // push the segment to sendList
    std::unique_lock<std::shared_mutex> lock(sendlst_m);
    // ASSERT_EQ(sendList.size(), 0, "sendList is not empty");
    sendList.push(ti);
    sendCv.notify_all();
    lock.unlock();
  }

  if (ti.nonblock) return 0;

  {  // wait for sender
    std::shared_lock<std::shared_mutex> lock(seq_m);
    seqCv.wait(lock, [&]() { return seq.snd_una > currSeq; });
    lock.unlock();

    std::unique_lock<std::mutex> aslock(abanseq_m);
    if (abandonedSeq.find(currSeq) != abandonedSeq.end()) {
      // failed TODO: reset
      abandonedSeq.erase(currSeq);
      RET_SETERRNO(ECONNRESET);
    }
    aslock.unlock();
  }
  return ti.ts.dataLen;
}

void TcpWorker::senderLoop() {
  /*
   * Send segment in sendList. If the top of queue with a different sequence
   * number from current number, it means that last segment has be sent
   * successfully. And the current sequence will be set to 0. Otherwise, sender
   * will try to send the segment again. abandonedSeq is used in `write`.
   *
   * If the currSeq is 0, the sender will get a new segment from list (without
   * poping). So it's not a problem though the sequence number is 0 itself.
   *
   * See more: send
   */
  std::shared_lock<std::shared_mutex> seqLock(seq_m, std::defer_lock);
  std::unique_lock<std::shared_mutex> sendLock(sendlst_m, std::defer_lock);
  std::unique_lock<std::mutex> abanseqLock(abanseq_m, std::defer_lock);
  TcpItem ti;
  int retransCnt = tcpMaxRetrans;
  tcp_seq currSeq = 0;

  while (true) {
    // wait for something if no sengment
    if (currSeq == 0) {
      sendLock.lock();
      sendCv.wait(sendLock, [&] { return sendList.size() > 0; });
      ti = sendList.front();
      sendLock.unlock();
      Printer::printTcpItem(ti, true);
      currSeq = ti.ts.hdr.th_seq;
    }
    Ip::sendIPPacket(ti.srcIp, ti.dstIp, IPPROTO_TCP, &ti.ts, ti.ts.totalLen);

    if (ti.nonblock) {
      // send next segment if nonblock
      retransCnt = tcpMaxRetrans;
      currSeq = 0;
      SENDLST_POP(sendLock);
      continue;
    }

    seqLock.lock();
    if (seqCv.wait_for(seqLock, std::chrono::seconds(tcpTimeout),
                       [&]() { return seq.snd_una > currSeq; })) {
      // send succeed
      currSeq = 0;
    } else {
      // send failed
      LOG_WARN("Send failed, rest retry: %d", retransCnt);
      if (retransCnt == 0) {
        retransCnt = tcpMaxRetrans;
        abanseqLock.lock();
        abandonedSeq.insert(currSeq);
        abanseqLock.unlock();
        SENDLST_POP(sendLock);
        currSeq = 0;
      } else {
        retransCnt--;
      }
    }
    seqLock.unlock();
    seqCv.notify_all();
  }
}

ssize_t TcpWorker::read(u_char* buf, size_t nby) {
  std::unique_lock<std::shared_mutex> lock(recvbuf_m);
  // TODO: if closed?
  recvCv.wait(lock, [&] { return recvBuf.size() > static_cast<int>(nby); });
  recvBuf.read(buf, nby);
  return nby;
}

void TcpWorker::handler(TcpItem& recvti) {
  while (st.load() != criticalSt.load())
    ;
  auto hdr = recvti.ts.hdr;
  // ATTENTION: src and dst matched local address!!
  Socket::SocketAddr srcSaddr(recvti.dstIp, hdr.th_dport);
  Socket::SocketAddr dstSaddr(recvti.srcIp, hdr.th_sport);
  std::unique_lock<std::shared_mutex> lock(sendlst_m, std::defer_lock);

  // send duplicate ACK if rcving a larger sequence number
  if (syned.load() && hdr.th_seq > seq.rcv_nxt) {
    LOG_INFO("Send an old ACK");
    auto ti = buildAckItem(srcSaddr, dstSaddr, seq, seq_m, seq.rcv_nxt);
    this->send(ti);
    return;
  }

  // send old ACK if rcving a smaller sequence number
  if (syned.load() && hdr.th_seq < seq.rcv_nxt) {
    LOG_INFO("Send a duplicated ACK");
    auto ti = buildAckItem(srcSaddr, dstSaddr, seq, seq_m,
                           hdr.th_seq + recvti.ts.dataLen);
    this->send(ti);
    return;
  }

  switch (getSt()) {
    case TcpState::LISTEN: {
      // LISTEN --[rcv SYN, snd SYN/ACK]--> SYN_RCVD > `accept`
      if (ISTYPE_SYN(hdr)) {
        if (backlog == 0 || static_cast<int>(pendings.size()) < backlog) {
          tcp_seq seq = recvti.ts.hdr.th_seq;
          pendings.push(std::make_pair(dstSaddr, seq));
          acceptCv.notify_all();
        }
      }
      break;
    }
    case TcpState::SYN_SENT: {
      // STN_SENT --[rcv SYN/ACK, snd ACK]--> ESTAB > `connect`
      if (ISTYPE_SYN_ACK(hdr)) {
        if (seq.tryAndRcvAck(hdr)) {
          seq.initRcvIsn(hdr.th_seq);
          SENDLST_POP_NOTIFY(lock);
          syned.store(true);
          setCriticalSt(TcpState::ESTABLISHED);
        }
        lock.unlock();
      }
      // SYN_SENT --[rcv SYN, snd SYN/ACK]--> SYN_RCVD > `connect`
      else if (ISTYPE_SYN(hdr)) {
        seq.initRcvIsn(hdr.th_seq);
        SENDLST_POP_NOTIFY(lock);
        setCriticalSt(TcpState::SYN_RECEIVED);
        lock.unlock();
      }
      break;
    }
    case TcpState::SYN_RECEIVED: {
      // SYN_RCVD --[rcv ACK]--> ESTAB > `connect` or `accept`
      if (ISTYPE_ACK(hdr)) {
        if (seq.tryAndRcvAck(hdr)) {
          SENDLST_POP_NOTIFY(lock);
          setCriticalSt(TcpState::ESTABLISHED);
        }
        lock.unlock();
      }
      break;
    }
    case TcpState::ESTABLISHED: {
      {  // get save and update rcv_nxt
        int len = recvti.ts.dataLen;
        if (len > 0) {
          recvBuf.write(recvti.ts.data, len);
          tcp_seq s = seq.sndAckWithLen(len);
          if (!WITHTYPE_FIN(hdr)) {
            auto ti = buildAckItem(srcSaddr, dstSaddr, seq, seq_m, s);
            this->send(ti);
          }
        }
      }  // end save data
      // ESTAB --[rcv FIN, snd ACK]--> CLOSE_WAIT
      if (ISTYPE_FIN(hdr)) {
        auto ti = buildAckItem(srcSaddr, dstSaddr, seq, seq_m);
        this->send(ti);
        setSt(TcpState::CLOSE_WAIT);
      }
      break;
    }
    case TcpState::FIN_WAIT_1: {
      {  // save data and update rcv_nxt
        int len = recvti.ts.dataLen;
        if (len > 0) {
          recvBuf.write(recvti.ts.data, len);
          tcp_seq s = seq.sndAckWithLen(len);
          if (!WITHTYPE_FIN(hdr)) {
            auto ti = buildAckItem(srcSaddr, dstSaddr, seq, seq_m, s);
            this->send(ti);
          }
        }
      }  // end save data
      // FIN_WAIT_1 --[rcv FIN/ACK, snd ACK]--> TIME_WAIT > `close`
      if (ISTYPE_FIN_ACK(hdr)) {
        if (seq.tryAndRcvAck(hdr)) {
          setCriticalSt(TcpState::TIMED_WAIT);
        }
      }
      // FIN_WAIT_1 --[rcv ACK]--> FIN_WAIT_2 > `close`
      else if (ISTYPE_ACK(hdr)) {
        if (seq.tryAndRcvAck(hdr)) {
          setCriticalSt(TcpState::FIN_WAIT_2);
        }
      }
      // FIN_WAIT_1 --[rcv FIN, snd ACK]--> CLOSING > `close`
      else if (ISTYPE_FIN(hdr)) {
        setCriticalSt(TcpState::CLOSING);
      }
      break;
    }
    case TcpState::FIN_WAIT_2: {
      // ^ FIN_WAIT_2 --[rcv FIN, snd ACK]--> TIME_WAIT > `close`
      if (ISTYPE_FIN(hdr)) {
        setCriticalSt(TcpState::TIMED_WAIT);
      }
      break;
    }
    case TcpState::CLOSING: {
      // ^ CLOSING --[rcv ACK]--> TIME_WAIT > `close`
      if (ISTYPE_ACK(hdr)) {
        setCriticalSt(TcpState::TIMED_WAIT);
      }
      break;
    }
    case TcpState::LAST_ACK: {
      // LAST_ACK --[rcv ACK]--> CLOSED > `close`
      if (ISTYPE_ACK(hdr)) {
        if (seq.tryAndRcvAck(hdr)) {
          setCriticalSt(TcpState::CLOSED);
        }
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace Tcp
