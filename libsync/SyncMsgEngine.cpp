/*
 * @CopyRight:
 * FISCO-BCOS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FISCO-BCOS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FISCO-BCOS.  If not, see <http://www.gnu.org/licenses/>
 * (c) 2016-2018 fisco-dev contributors.
 */
/**
 * @brief : The implementation of callback from p2p
 * @author: jimmyshi
 * @date: 2018-10-17
 */
#include "SyncMsgEngine.h"

using namespace std;
using namespace dev;
using namespace dev::eth;
using namespace dev::sync;
using namespace dev::p2p;
using namespace dev::blockchain;
using namespace dev::txpool;

void SyncMsgEngine::messageHandler(
    P2PException _e, std::shared_ptr<dev::p2p::Session> _session, Message::Ptr _msg)
{
    if (!checkSession(_session))
    {
        _session->disconnect(LocalIdentity);
        return;
    }

    SyncMsgPacket packet;
    if (!packet.decode(_session, _msg))
    {
        LOG(WARNING) << "Received " << _msg->buffer()->size() << ": " << toHex(*_msg->buffer())
                     << endl;
        LOG(WARNING) << "INVALID MESSAGE RECEIVED";
        _session->disconnect(BadProtocol);
        return;
    }

    bool ok = interpret(packet);
    if (!ok)
        LOG(WARNING) << "Couldn't interpret packet. " << RLP(packet.rlp());
}

bool SyncMsgEngine::checkSession(std::shared_ptr<dev::p2p::Session> _session)
{
    /// TODO: denine LocalIdentity after SyncPeer finished
    //_session->id();
    return true;
}

bool SyncMsgEngine::checkPacket(bytesConstRef _msg)
{
    if (_msg[0] > 0x7f || _msg.size() < 2)
        return false;
    if (RLP(_msg.cropped(1)).actualSize() + 1 != _msg.size())
        return false;
    return true;
}

bool SyncMsgEngine::interpret(SyncMsgPacket const& _packet)
{
    switch (_packet.packetType)
    {
    case StatusPacket:
        onPeerStatus(_packet);
        break;
    case TransactionsPacket:
        onPeerTransactions(_packet);
        break;
    case BlocksPacket:
        onPeerBlocks(_packet);
        break;
    case ReqBlocskPacket:
        onPeerRequestBlocks(_packet);
        break;
    default:
        return false;
    }
    return true;
}

void SyncMsgEngine::onPeerStatus(SyncMsgPacket const& _packet)
{
    shared_ptr<SyncPeerStatus> status = m_syncStatus->peerStatus(_packet.nodeId);
    NodeInfo info{_packet.nodeId, _packet.rlp()[0].toInt<int64_t>(),
        _packet.rlp()[1].toHash<h256>(), _packet.rlp()[2].toHash<h256>()};

    if (status == nullptr)
        m_syncStatus->newSyncPeerStatus(info);
    else
        status->update(info);
}

void SyncMsgEngine::onPeerTransactions(SyncMsgPacket const& _packet)
{
    RLP const& rlps = _packet.rlp();
    unsigned itemCount = rlps.itemCount();

    for (unsigned i = 0; i < itemCount; ++i)
    {
        Transaction tx;
        tx.decode(rlps[i]);

        if (ImportResult::Success == m_txPool->import(tx))
        {
            LOG(TRACE) << "Import transaction " << tx.sha3() << " from peer " << _packet.nodeId;
        }

        auto p_tx = m_txPool->transactionInPool(tx.sha3());
        if (p_tx != nullptr)
            p_tx->addImportPeer(_packet.nodeId);
    }
}

void SyncMsgEngine::onPeerBlocks(SyncMsgPacket const& _packet)
{
    RLP const& rlps = _packet.rlp();
    unsigned itemCount = rlps.itemCount();
    for (unsigned i = 0; i < itemCount; ++i)
    {
        shared_ptr<Block> block = make_shared<Block>(rlps[i].toBytes());
        // TODO check minerlist sig
        m_syncStatus->bq().push(block);
    }
}

void SyncMsgEngine::onPeerRequestBlocks(SyncMsgPacket const& _packet)
{
    RLP const& rlp = _packet.rlp();

    // request
    int64_t from = rlp[0].toInt<int64_t>();
    unsigned size = rlp[1].toInt<unsigned>();

    vector<bytes> blockRLPs;
    for (int64_t number = from; number < from + size; ++number)
    {
        shared_ptr<Block> block = m_blockChain->getBlockByNumber(number);
        if (!block || block->header().number() != number)
            break;
        blockRLPs.emplace_back(block->rlp());
    }

    SyncBlocksPacket retPacket;
    retPacket.encode(blockRLPs);

    m_service->asyncSendMessageByNodeID(_packet.nodeId, retPacket.toMessage(m_protocolId));
    LOG(TRACE) << "Send blocks [" << from << ", " << from + blockRLPs.size() << "] to "
               << _packet.nodeId;
}