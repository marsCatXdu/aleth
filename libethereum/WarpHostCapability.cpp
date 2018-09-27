/*
    This file is part of cpp-ethereum.

    cpp-ethereum is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    cpp-ethereum is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "WarpHostCapability.h"
#include "BlockChain.h"

#include <boost/fiber/all.hpp>
#include <chrono>

namespace dev
{
namespace eth
{
namespace
{
static size_t const c_freePeerBufferSize = 32;

bool validateManifest(RLP const& _manifestRlp)
{
    if (!_manifestRlp.isList() || _manifestRlp.itemCount() != 1)
        return false;

    RLP const manifest = _manifestRlp[0];

    u256 const version = manifest[0].toInt<u256>();
    return version == 2;
}

h256 snapshotBlockHash(RLP const& _manifestRlp)
{
    RLP const manifest = _manifestRlp[0];
    return manifest[5].toHash<h256>();
}

class WarpPeerObserver : public WarpPeerObserverFace
{
public:
    WarpPeerObserver(WarpHostCapability const& _host, BlockChain const& _blockChain,
        boost::filesystem::path const& _snapshotPath)
      : m_hostProtocolVersion(_host.protocolVersion()),
        m_hostNetworkId(_host.networkId()),
        m_hostGenesisHash(_blockChain.genesisHash()),
        m_daoForkBlock(_blockChain.sealEngine()->chainParams().daoHardforkBlock),
        m_freePeers(c_freePeerBufferSize),
        m_snapshotDir(_snapshotPath)
    {}
    ~WarpPeerObserver()
    {
        if (m_downloadFiber)
            m_downloadFiber->join();
    }

    void onPeerStatus(std::shared_ptr<WarpPeerCapability> _peer) override
    {
        boost::fibers::fiber checkPeerFiber(
            &WarpPeerObserver::validatePeer, this, std::move(_peer));
        checkPeerFiber.detach();

        // start the downloading fiber in the thread handling network messages
        if (!m_downloadFiber)
            m_downloadFiber.reset(
                new boost::fibers::fiber(&WarpPeerObserver::downloadChunks, this));

        boost::this_fiber::yield();
    }

    void onPeerManifest(std::shared_ptr<WarpPeerCapability> _peer, RLP const& _r) override
    {
        m_manifests[_peer].set_value(_r.data().toBytes());
        boost::this_fiber::yield();
    }

    void onPeerBlockHeaders(std::shared_ptr<WarpPeerCapability> _peer, RLP const& _r) override
    {
        m_daoForkHeaders[_peer].set_value(_r.data().toBytes());
        boost::this_fiber::yield();
    }

    void onPeerData(std::shared_ptr<WarpPeerCapability> _peer, RLP const& _r) override
    {
        if (!_r.isList() || _r.itemCount() != 1)
            return;

        RLP const data = _r[0];

        h256 const hash = sha3(data.toBytesConstRef());

        auto it = m_requestedChunks.find(_peer);
        if (it == m_requestedChunks.end())
            return;

        h256 const askedHash = it->second;
        m_requestedChunks.erase(it);

        if (hash == askedHash)
        {
            // TODO handle writeFile failure
            writeFile((boost::filesystem::path(m_snapshotDir) / toHex(hash)).string(),
                data.toBytesConstRef());

            LOG(m_logger) << "Saved chunk " << hash << " Chunks left: " << m_neededChunks.size()
                          << " Requested chunks: " << m_requestedChunks.size();
            if (m_neededChunks.empty() && m_requestedChunks.empty())
                LOG(m_logger) << "Snapshot download complete!";
        }
        else
            m_neededChunks.push_back(askedHash);

        m_freePeers.push(_peer);
        boost::this_fiber::yield();
    }

    void onPeerDisconnect(std::shared_ptr<WarpPeerCapability> _peer, Asking _asking) override
    {
        if (_asking == Asking::WarpManifest)
        {
            auto it = m_manifests.find(_peer);
            if (it != m_manifests.end())
                it->second.set_exception(std::make_exception_ptr(FailedToDownloadManifest()));
        }
        else if (_asking == Asking::BlockHeaders)
        {
            auto it = m_daoForkHeaders.find(_peer);
            if (it != m_daoForkHeaders.end())
                it->second.set_exception(
                    std::make_exception_ptr(FailedToDownloadDaoForkBlockHeader()));
        }
        else if (_asking == Asking::WarpData)
        {
            auto it = m_requestedChunks.find(_peer);
            if (it != m_requestedChunks.end())
            {
                m_neededChunks.push_back(it->second);
                m_requestedChunks.erase(it);
            }
        }
        boost::this_fiber::yield();
    }

private:
    void validatePeer(std::shared_ptr<WarpPeerCapability> _peer)
    {
        if (!_peer->validateStatus(m_hostGenesisHash, {m_hostProtocolVersion}, m_hostNetworkId))
            return;

        _peer->requestManifest();

        bytes const manifestBytes = waitForManifestResponse(_peer);
        if (manifestBytes.empty())
            return;

        RLP manifestRlp(manifestBytes);
        if (!validateManifest(manifestRlp))
        {
            // TODO try disconnecting instead of disabling; disabled peer still occupies the peer slot
            _peer->disable("Invalid snapshot manifest.");
            return;
        }

        u256 const snapshotHash = snapshotBlockHash(manifestRlp);
        if (m_syncingSnapshotHash)
        {
            if (snapshotHash == m_syncingSnapshotHash)
                m_freePeers.push(_peer);
            else
                _peer->disable("Another snapshot.");
        }
        else
        {
            if (m_daoForkBlock)
            {
                _peer->requestBlockHeaders(m_daoForkBlock, 1, 0, false);

                bytes const headerBytes = waitForDaoForkBlockResponse(_peer);
                if (headerBytes.empty())
                    return;

                RLP headerRlp(headerBytes);
                if (!verifyDaoChallengeResponse(headerRlp))
                {
                    _peer->disable("Peer from another fork.");
                    return;
                }
            }

            m_syncingSnapshotHash = snapshotHash;
            m_manifest.set_value(manifestBytes);
            m_freePeers.push(_peer);
        }
    }

    bytes waitForManifestResponse(std::weak_ptr<WarpPeerCapability> _peer)
    {
        try
        {
            bytes const result = m_manifests[_peer].get_future().get();
            m_manifests.erase(_peer);
            return result;
        }
        catch (Exception const&)
        {
            m_manifests.erase(_peer);
        }
        return bytes{};
    }

    bytes waitForDaoForkBlockResponse(std::weak_ptr<WarpPeerCapability> _peer)
    {
        try
        {
            bytes const result = m_daoForkHeaders[_peer].get_future().get();
            m_daoForkHeaders.erase(_peer);
            return result;
        }
        catch (Exception const&)
        {
            m_daoForkHeaders.erase(_peer);
        }
        return bytes{};
    }

    bool verifyDaoChallengeResponse(RLP const& _r)
    {
        if (_r.itemCount() != 1)
            return false;

        BlockHeader info(_r[0].data(), HeaderData);
        return info.number() == m_daoForkBlock &&
               info.extraData() == fromHex("0x64616f2d686172642d666f726b");
    }

    void downloadChunks()
    {
        bytes const manifestBytes = m_manifest.get_future().get();

        RLP manifestRlp(manifestBytes);
        RLP manifest(manifestRlp[0]);

        u256 const version = manifest[0].toInt<u256>();
        h256s const stateHashes = manifest[1].toVector<h256>();
        h256s const blockHashes = manifest[2].toVector<h256>();
        h256 const stateRoot = manifest[3].toHash<h256>();
        u256 const blockNumber = manifest[4].toInt<u256>();
        h256 const blockHash = manifest[5].toHash<h256>();

        LOG(m_logger) << "MANIFEST: "
                      << "version " << version << " state root " << stateRoot << " block number "
                      << blockNumber << " block hash " << blockHash;

        // TODO handle writeFile failure
        writeFile((boost::filesystem::path(m_snapshotDir) / "MANIFEST").string(), manifest.data());

        m_neededChunks.assign(stateHashes.begin(), stateHashes.end());
        m_neededChunks.insert(m_neededChunks.end(), blockHashes.begin(), blockHashes.end());

        while (!m_neededChunks.empty())
        {
            h256 const chunkHash(m_neededChunks.front());

            std::shared_ptr<WarpPeerCapability> peer;
            while (!peer)
                peer = m_freePeers.value_pop().lock();

            LOG(m_logger) << "Requesting chunk " << chunkHash;
            peer->requestData(chunkHash);

            m_requestedChunks[peer] = chunkHash;
            m_neededChunks.pop_front();
        }
    }

    unsigned const m_hostProtocolVersion;
    u256 const m_hostNetworkId;
    h256 const m_hostGenesisHash;
    unsigned const m_daoForkBlock;
    boost::fibers::promise<bytes> m_manifest;
    h256 m_syncingSnapshotHash;
    std::deque<h256> m_neededChunks;
    boost::fibers::buffered_channel<std::weak_ptr<WarpPeerCapability>> m_freePeers;
    boost::filesystem::path const m_snapshotDir;
    std::map<std::weak_ptr<WarpPeerCapability>, boost::fibers::promise<bytes>,
        std::owner_less<std::weak_ptr<WarpPeerCapability>>>
        m_manifests;
    std::map<std::weak_ptr<WarpPeerCapability>, boost::fibers::promise<bytes>,
        std::owner_less<std::weak_ptr<WarpPeerCapability>>>
        m_daoForkHeaders;
    std::map<std::weak_ptr<WarpPeerCapability>, h256,
        std::owner_less<std::weak_ptr<WarpPeerCapability>>>
        m_requestedChunks;

    std::unique_ptr<boost::fibers::fiber> m_downloadFiber;

    Logger m_logger{createLogger(VerbosityInfo, "snap")};
};

}  // namespace


WarpHostCapability::WarpHostCapability(std::shared_ptr<p2p::CapabilityHostFace> _host, BlockChain const& _blockChain,
    u256 const& _networkId, boost::filesystem::path const& _snapshotDownloadPath,
    std::shared_ptr<SnapshotStorageFace> _snapshotStorage)
  : m_host(std::move(_host)),
    m_blockChain(_blockChain),
    m_networkId(_networkId),
    m_snapshot(_snapshotStorage),
    // observer needed only in case we download snapshot
    m_peerObserver(
        _snapshotDownloadPath.empty() ? nullptr : createPeerObserver(_snapshotDownloadPath)),
    m_lastTick(0)
{
}

WarpHostCapability::~WarpHostCapability()
{
    terminate();
}

std::shared_ptr<WarpPeerObserverFace> WarpHostCapability::createPeerObserver(
    boost::filesystem::path const& _snapshotDownloadPath) const
{
    return std::make_shared<WarpPeerObserver>(*this, m_blockChain, _snapshotDownloadPath);
}
/*
std::shared_ptr<p2p::PeerCapabilityFace> WarpHostCapability::newPeerCapability(
    std::shared_ptr<p2p::SessionFace> const& _s, unsigned _idOffset, p2p::CapDesc const& _cap)
{
    auto ret = HostCapability<WarpPeerCapability>::newPeerCapability(_s, _idOffset, _cap);

    auto cap = p2p::capabilityFromSession<WarpPeerCapability>(*_s, _cap.second);
    assert(cap);
    cap->init(c_WarpProtocolVersion, m_networkId, m_blockChain.details().totalDifficulty,
        m_blockChain.currentHash(), m_blockChain.genesisHash(), m_snapshot, m_peerObserver);

    return ret;
}
*/
void WarpHostCapability::doWork()
{
    time_t const now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    if (now - m_lastTick >= 1)
    {
        m_lastTick = now;

        for (auto const& peer : m_peers)
        {
            time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            auto const& status = peer.second;
            if (now - status.m_lastAsk > 10 && status.m_asking != Asking::Nothing)
            {
                // timeout
                m_host->disconnect(peer.first, p2p::PingTimeout);
            }
        }
    }
}

void WarpHostCapability::onConnect(p2p::NodeID const& _peerID, u256 const& /* _peerCapabilityVersion */)
{
    m_peers.emplace(_peerID, WarpPeerStatus{});

    u256 snapshotBlockNumber;
    h256 snapshotBlockHash;
    if (m_snapshot)
    {
        bytes const snapshotManifest(m_snapshot->readManifest());
        RLP manifest(snapshotManifest);
        if (manifest.itemCount() != 6)
            BOOST_THROW_EXCEPTION(InvalidSnapshotManifest());
        snapshotBlockNumber = manifest[4].toInt<u256>(RLP::VeryStrict);
        snapshotBlockHash = manifest[5].toHash<h256>(RLP::VeryStrict);
    }

    requestStatus(_peerID, m_networkId, m_blockChain.details().totalDifficulty,
        m_blockChain.currentHash(), m_blockChain.genesisHash(), snapshotBlockHash, snapshotBlockNumber);
}

bool WarpHostCapability::interpretCapabilityPacket(p2p::NodeID const& _nodeID, unsigned _id, RLP const&)
{
    std::shared_ptr<WarpPeerObserverFace> observer(m_observer.lock());
    // TODO: we still want to answer some messages when we only give out imported snapshot
    if (!observer)
        return false;

    m_lastAsk = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    try
    {
        switch (_id)
        {
        case WarpStatusPacket:
        {
            if (_r.itemCount() < 7)
                BOOST_THROW_EXCEPTION(InvalidWarpStatusPacket());

            // Packet layout:
            // [ version:P, state_hashes : [hash_1:B_32, hash_2 : B_32, ...],  block_hashes :
            // [hash_1:B_32, hash_2 : B_32, ...],
            //      state_root : B_32, block_number : P, block_hash : B_32 ]
            m_protocolVersion = _r[0].toInt<unsigned>();
            m_networkId = _r[1].toInt<u256>();
            m_totalDifficulty = _r[2].toInt<u256>();
            m_latestHash = _r[3].toHash<h256>();
            m_genesisHash = _r[4].toHash<h256>();
            m_snapshotHash = _r[5].toHash<h256>();
            m_snapshotNumber = _r[6].toInt<u256>();

            cnetlog << "Status: "
                << " protocol version " << m_protocolVersion << " networkId " << m_networkId
                << " genesis hash " << m_genesisHash << " total difficulty "
                << m_totalDifficulty << " latest hash " << m_latestHash << " snapshot hash "
                << m_snapshotHash << " snapshot number " << m_snapshotNumber;
            setIdle();
            observer->onPeerStatus(
                std::dynamic_pointer_cast<WarpPeerCapability>(shared_from_this()));
            break;
        }
        case GetSnapshotManifest:
        {
            if (!m_snapshot)
                return false;

            RLPStream s;
            prep(s, SnapshotManifest, 1).appendRaw(m_snapshot->readManifest());
            sealAndSend(s);
            break;
        }
        case GetSnapshotData:
        {
            if (!m_snapshot)
                return false;

            const h256 chunkHash = _r[0].toHash<h256>(RLP::VeryStrict);

            RLPStream s;
            prep(s, SnapshotData, 1).append(m_snapshot->readCompressedChunk(chunkHash));
            sealAndSend(s);
            break;
        }
        case GetBlockHeadersPacket:
        {
            // TODO We are being asked DAO fork block sometimes, need to be able to answer this
            RLPStream s;
            prep(s, BlockHeadersPacket);
            sealAndSend(s);
            break;
        }
        case BlockHeadersPacket:
        {
            setIdle();
            observer->onPeerBlockHeaders(
                (std::dynamic_pointer_cast<WarpPeerCapability>(shared_from_this())), _r);
            break;
        }
        case SnapshotManifest:
        {
            setIdle();
            observer->onPeerManifest(
                (std::dynamic_pointer_cast<WarpPeerCapability>(shared_from_this())), _r);
            break;
        }
        case SnapshotData:
        {
            setIdle();
            observer->onPeerData(
                (std::dynamic_pointer_cast<WarpPeerCapability>(shared_from_this())), _r);
            break;
        }
        default:
            return false;
        }
    }
    catch (Exception const&)
    {
        cnetlog << "Warp Peer causing an Exception: "
            << boost::current_exception_diagnostic_information() << " " << _r;
    }
    catch (std::exception const& _e)
    {
        cnetlog << "Warp Peer causing an exception: " << _e.what() << " " << _r;
    }

    return true;
}

void WarpHostCapability::onDisconnect(p2p::NodeID const& _peerID)
{
    m_peers.erase(_peerID);
}


void WarpHostCapability::requestStatus(p2p::NodeID const& _peerID, unsigned _hostProtocolVersion, u256 const& _hostNetworkId,
    u256 const& _chainTotalDifficulty, h256 const& _chainCurrentHash, h256 const& _chainGenesisHash,
    h256 const& _snapshotBlockHash, u256 const& _snapshotBlockNumber)
{
    RLPStream s;
    prep(s, WarpStatusPacket, 7) << _hostProtocolVersion << _hostNetworkId << _chainTotalDifficulty
        << _chainCurrentHash << _chainGenesisHash << _snapshotBlockHash
        << _snapshotBlockNumber;
    sealAndSend(s);
}


void WarpHostCapability::requestBlockHeaders(
    p2p::NodeID const& _peerID, unsigned _startNumber, unsigned _count, unsigned _skip, bool _reverse)
{
    assert(m_asking == Asking::Nothing);
    setAsking(Asking::BlockHeaders);
    RLPStream s;
    prep(s, GetBlockHeadersPacket, 4) << _startNumber << _count << _skip << (_reverse ? 1 : 0);
    sealAndSend(s);
}

void WarpHostCapability::requestManifest(p2p::NodeID const& _peerID)
{
    assert(m_asking == Asking::Nothing);
    setAsking(Asking::WarpManifest);
    RLPStream s;
    prep(s, GetSnapshotManifest);
    sealAndSend(s);
}

void WarpHostCapability::requestData(p2p::NodeID const& _peerID, h256 const& _chunkHash)
{
    assert(m_asking == Asking::Nothing);
    setAsking(Asking::WarpData);
    RLPStream s;
    prep(s, GetSnapshotData, 1) << _chunkHash;
    sealAndSend(s);
}

void WarpHostCapability::setAsking(p2p::NodeID const& _peerID, Asking _a)
{
    m_asking = _a;
    m_lastAsk = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

}  // namespace eth
}  // namespace dev
