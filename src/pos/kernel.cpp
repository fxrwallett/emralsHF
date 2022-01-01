// Copyright (c) 2012-2019 The Peercoin developers
// Copyright (c) 2017-2019 The Bit Green Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <db.h>
#include <chainparams.h>
#include <pos/kernel.h>
#include <script/interpreter.h>
#include <timedata.h>
#include <wallet/wallet.h>
#include <policy/policy.h>
#include <init.h>
#include <validation.h>
#include <index/txindex.h>
#include <util/time.h>

// Hard checkpoints of stake modifiers to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints = {
                                                                     {     0, 0x0e00670b},
                                                                     { 10000, 0x5cfbce9d},
                                                                     { 20000, 0xc16b56d1},
                                                                     { 40000, 0xb9778a75},
                                                                     { 60000, 0x20607fd3},
                                                                     { 80000, 0x5ea04d7d},
                                                                     {100000, 0x8c5032f4},
                                                                     {120000, 0x02f548e8},
                                                                     {140000, 0xa6ce0a3c},
                                                                     {160000, 0xfdb8036e},
                                                                     {180000, 0xa0987af6},
                                                                     {200000, 0x3ab8869c},
                                                                     {220000, 0x06de4abd},
                                                                     {240000, 0x835b2471},
                                                                     {260000, 0x9b40c935},
                                                                     {280000, 0xd07f22c6}
                                                                 };

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("%s: null pindex", __func__);
    do {
        if (pindex->GeneratedStakeModifier()) {
            nStakeModifier = pindex->nStakeModifier;
            nModifierTime = pindex->GetBlockTime();
            return true;
        }
    } while ((pindex = pindex->pprev) != nullptr);
    return error("%s: no generation at genesis block", __func__);
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert (nSection >= 0 && nSection < 64);
    return (Params().GetConsensus().nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection = 0; nSection < 64; nSection++)
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(
    std::vector<std::pair<int64_t, uint256> >& vSortedByTimestamp,
    std::map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop,
    uint64_t nStakeModifierPrev,
    const CBlockIndex** pindexSelected)
{
    bool fSelected = false;
    arith_uint256 hashBest = 0;
    *pindexSelected = (const CBlockIndex*) 0;
    for (const auto& item : vSortedByTimestamp)
    {
        if (!::BlockIndex().count(item.second))
            return error("%s: failed to find block index for candidate block %s", __func__, item.second.ToString());
        const CBlockIndex* pindex = ::BlockIndex()[item.second];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;
        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;
        // compute the selection hash by hashing its proof-hash and the
        // previous proof-of-stake modifier
        uint256 hashProof = pindex->IsProofOfStake() ? pindex->hashProofOfStake : pindex->GetBlockHash();
        CDataStream ss(SER_GETHASH, 0);
        ss << hashProof << nStakeModifierPrev;
        arith_uint256 hashSelection = UintToArith256(Hash(ss.begin(), ss.end()));
        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;
        if (fSelected && hashSelection < hashBest)
        {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
        else if (!fSelected)
        {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
    }
    LogPrint(BCLog::KERNEL, "%s: selection hash=%s\n", __func__, hashBest.ToString());
    return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexCurrent, uint64_t &nStakeModifier, bool& fGeneratedStakeModifier)
{
    const Consensus::Params& params = Params().GetConsensus();
    const CBlockIndex* pindexPrev = pindexCurrent->pprev;
    nStakeModifier = 0;
    fGeneratedStakeModifier = false;
    if (!pindexPrev)
    {
        fGeneratedStakeModifier = true;
        return true;  // genesis block's modifier is 0
    }
    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
        return error("%s: unable to get last modifier", __func__);

    LogPrint(BCLog::KERNEL, "%s: prev modifier=0x%016x time=%s epoch=%u\n", __func__, nStakeModifier, FormatISO8601DateTime(nModifierTime), (unsigned int)nModifierTime);
    if (nModifierTime / params.nModifierInterval >= pindexPrev->GetBlockTime() / params.nModifierInterval)
    {
        LogPrint(BCLog::KERNEL, "%s: no new interval keep current modifier: pindexPrev nHeight=%d nTime=%u\n",
            __func__, pindexPrev->nHeight, (unsigned int)pindexPrev->GetBlockTime());
        return true;
    }
    if (nModifierTime / params.nModifierInterval >= pindexCurrent->GetBlockTime() / params.nModifierInterval)
    {
        LogPrint(BCLog::KERNEL, "%s: no new interval keep current modifier: pindexCurrent nHeight=%d nTime=%u\n",
            __func__, pindexCurrent->nHeight, (unsigned int)pindexCurrent->GetBlockTime());
        return true;
    }

    // Sort candidate blocks by timestamp
    std::vector<std::pair<int64_t, uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * params.nModifierInterval / params.nPosTargetTimespan);
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / params.nModifierInterval) * params.nModifierInterval - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;
    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart)
    {
        vSortedByTimestamp.push_back(std::make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }
    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;

    reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
    sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    std::map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound=0; nRound<std::min(64, (int)vSortedByTimestamp.size()); nRound++)
    {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex))
            return error("%s: unable to select block at round %d", __func__, nRound);
        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);
        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(std::make_pair(pindex->GetBlockHash(), pindex));
        LogPrint(BCLog::KERNEL, "%s: selected round %d stop=%s height=%d bit=%d\n",
            __func__, nRound, FormatISO8601DateTime(nSelectionIntervalStop), pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (gArgs.IsArgSet("-debug"))
    {
        std::string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate)
        {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        for (const auto& item : mapSelectedBlocks)
        {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake()? "S" : "W");
        }
        LogPrint(BCLog::KERNEL, "%s: selection height [%d, %d] map %s\n", __func__, nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap);
    }

    LogPrint(BCLog::KERNEL, "%s: new modifier=0x%016x time=%s\n", __func__, nStakeModifierNew, FormatISO8601DateTime(pindexPrev->GetBlockTime()));

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

// V0.3: Stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
static bool GetKernelStakeModifierV03(CBlockIndex* pindexPrev, uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    const Consensus::Params& params = Params().GetConsensus();
    nStakeModifier = 0;
    if (!::BlockIndex().count(hashBlockFrom))
        return error("GetKernelStakeModifier() : block not indexed");
    const CBlockIndex* pindexFrom = ::BlockIndex()[hashBlockFrom];
    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();

    // we need to iterate index forward but we cannot depend on ChainActive().Next()
    // because there is no guarantee that we are checking blocks in active chain.
    // So, we construct a temporary chain that we will iterate over.
    // pindexFrom - this block contains coins that are used to generate PoS
    // pindexPrev - this is a block that is previous to PoS block that we are checking, you can think of it as tip of our chain
    std::vector<CBlockIndex*> tmpChain;
    int32_t nDepth = pindexPrev->nHeight - (pindexFrom->nHeight-1); // -1 is used to also include pindexFrom
    tmpChain.reserve(nDepth);
    CBlockIndex* it = pindexPrev;
    for (int i=1; i<=nDepth && !ChainActive().Contains(it); i++) {
        tmpChain.push_back(it);
        it = it->pprev;
    }
    std::reverse(tmpChain.begin(), tmpChain.end());
    size_t n = 0;

    const CBlockIndex* pindex = pindexFrom;
    // loop to find the stake modifier later by a selection interval
    while (nStakeModifierTime < pindexFrom->GetBlockTime() + nStakeModifierSelectionInterval)
    {
        const CBlockIndex* old_pindex = pindex;
        pindex = (!tmpChain.empty() && pindex->nHeight >= tmpChain[0]->nHeight - 1)? tmpChain[n++] : ChainActive().Next(pindex);
        if (n > tmpChain.size() || pindex == nullptr) // check if tmpChain[n+1] exists
        {   // reached best block; may happen if node is behind on block chain
            if (fPrintProofOfStake || (old_pindex->GetBlockTime() + params.nStakeMinAge - nStakeModifierSelectionInterval > GetAdjustedTime()))
                return error("GetKernelStakeModifier() : reached best block %s at height %d from block %s",
                    old_pindex->GetBlockHash().ToString(), old_pindex->nHeight, hashBlockFrom.ToString());
            else
                return false;
        }
        if (pindex->GeneratedStakeModifier())
        {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
    }
    nStakeModifier = pindex->nStakeModifier;
    return true;
}

// Get the stake modifier specified by the protocol to hash for a stake kernel
static bool GetKernelStakeModifier(CBlockIndex* pindexPrev, uint256 hashBlockFrom, unsigned int nTimeTx, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    return GetKernelStakeModifierV03(pindexPrev, hashBlockFrom, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake);
}

// peercoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.offset + txPrev.nTime + txPrev.vout.n + nTime) < bnTarget * nCoinDayWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier:
//       (v0.5) uses dynamic stake modifier around 21 days before the kernel,
//              versus static stake modifier about 9 days after the staked
//              coin (txPrev) used in v0.3
//       (v0.3) scrambles computation to make it very difficult to precompute
//              future proof-of-stake at the time of the coin's confirmation
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.offset: offset of txPrev inside block, to reduce the chance of
//                  nodes generating coinstake at the same time
//   txPrev.nTime: reduce the chance of nodes generating coinstake at the same
//                 time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(unsigned int nBits, CBlockIndex* pindexPrev, const CBlockHeader blockFrom, const CTransactionRef& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, bool fPrintProofOfStake)
{
    const Consensus::Params& params = Params().GetConsensus();
    bool fHardenedChecks = pindexPrev->nHeight+1 > params.StakeEnforcement();

    auto txPrevTime = blockFrom.GetBlockTime();
    if (nTimeTx < txPrevTime) {
        //! mimic legacy behaviour
        if (!fHardenedChecks) {
            return error("%s: nTime violation", __func__);
        } else {
            return error("%s: timestamp violation (nTimeTx < txPrevTime)", __func__);
        }
    }

    unsigned int nTimeBlockFrom = blockFrom.GetBlockTime();
    if (nTimeBlockFrom + params.nStakeMinAge > nTimeTx) {
        //! mimic legacy behaviour
        if (!fHardenedChecks) {
            return error("%s: min age violation", __func__);
        } else {
            return error("%s: min age violation (nTimeBlockFrom + params.nStakeMinAge > nTimeTx)", __func__);
        }
    }

    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);
    CAmount nValueIn = txPrev->vout[prevout.n].nValue;

    //! enforce minimum stake amount
    if (nValueIn < Params().GetConsensus().MinStakeAmount() && fHardenedChecks) {
        LogPrintf("Minimum stake amount is %d, amount found was %d\n", Params().GetConsensus().MinStakeAmount()/COIN, nValueIn/COIN);
        return false;
    }

    // v0.3 protocol kernel hash weight starts from 0 at the 30-day min age
    // this change increases active coins participating the hash and helps
    // to secure the network when proof-of-stake difficulty is low
    int64_t nTimeWeight = std::min<int64_t>(nTimeTx - txPrevTime, params.nStakeMaxAge - params.nStakeMinAge);
    arith_uint256 bnCoinDayWeight = nValueIn * nTimeWeight / COIN / 200;
    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    uint64_t nStakeModifier = 0;
    int nStakeModifierHeight = 0;
    int64_t nStakeModifierTime = 0;

    if (!GetKernelStakeModifier(pindexPrev, blockFrom.GetHash(), nTimeTx, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, false))
        return false;

    ss << nStakeModifier;
    ss << nTimeBlockFrom << txPrevTime << prevout.n << nTimeTx;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    // Now check if proof-of-stake hash meets target protocol
    LogPrint(BCLog::KERNEL, "%s: nValueIn=%s hashProofOfStake=%s hashTarget=%s\n", __func__, FormatMoney(nValueIn), hashProofOfStake.ToString(), (bnCoinDayWeight * bnTargetPerCoinDay).ToString());

    if (UintToArith256(hashProofOfStake) > bnCoinDayWeight * bnTargetPerCoinDay)
        return false;

    LogPrint(BCLog::KERNEL, "%s: using modifier 0x%016x at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
        __func__, nStakeModifier, nStakeModifierHeight,
        FormatISO8601DateTime(nStakeModifierTime),
        ::BlockIndex()[blockFrom.GetHash()]->nHeight,
        FormatISO8601DateTime(blockFrom.GetBlockTime()));

    LogPrint(BCLog::KERNEL, "%s: modifier=0x%016x nTimeBlockFrom=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
        __func__,
        nStakeModifier,
        nTimeBlockFrom, txPrevTime, prevout.n, nTimeTx,
        hashProofOfStake.ToString());

    return true;
}

int GetLastHeight(uint256 txHash)
{
    uint256 hashBlock;
    CTransactionRef stakeInput;
    if (!GetTransaction(txHash, stakeInput, Params().GetConsensus(), hashBlock))
        return 0;

    if (hashBlock == uint256())
        return 0;

    return ::LookupBlockIndex(hashBlock)->nHeight;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(const CBlock &block, CBlockIndex* pindexPrev, uint256& hashProofOfStake)
{
    const Consensus::Params& params = Params().GetConsensus();
    bool fHardenedChecks = pindexPrev->nHeight+1 > params.StakeEnforcement();

    const CTransactionRef &tx = block.vtx[1];
    if (!tx->IsCoinStake())
        return error("%s: called on non-coinstake %s", __func__, tx->GetHash().ToString());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx->vin[0];

    // Transaction index is required to get to block header
    if (!g_txindex)
        return error("%s: transaction index not available", __func__);

    // First try finding the previous transaction in database
    uint256 hashBlock;
    CTransactionRef txPrev;
    if (!GetTransaction(txin.prevout.hash, txPrev, Params().GetConsensus(), hashBlock))
        return error("%s: read txPrev failed", __func__);

    // Enforce minimum stake depth
    const int nPreviousBlockHeight = pindexPrev->nHeight;
    const int nBlockFromHeight = GetLastHeight(txin.prevout.hash);

    // returning zero from GetLastHeight() indicates error
    if (nBlockFromHeight == 0 && fHardenedChecks)
        return error("%s: returning zero from GetLastHeight()", __func__);

    if (!Params().GetConsensus().HasStakeMinDepth(nPreviousBlockHeight+1, nBlockFromHeight) && fHardenedChecks) {
        LogPrintf("\n%s : min age violation - height=%d - nHeightBlockFrom=%d (depth=%d)\n", __func__, nPreviousBlockHeight, nBlockFromHeight, nPreviousBlockHeight - nBlockFromHeight);
        return false;
    }

    CBlockHeader header = LookupBlockIndex(hashBlock)->GetBlockHeader();

    // Verify signature
    {
        const CTxOut& prevOut = txPrev->vout[txin.prevout.n];
        TransactionSignatureChecker checker(&(*tx), 0, prevOut.nValue, PrecomputedTransactionData(*tx));

        if (!VerifyScript(txin.scriptSig, prevOut.scriptPubKey, &(txin.scriptWitness), SCRIPT_VERIFY_P2SH, checker, nullptr))
            return error("%s: check kernel script failed on coinstake %s, hashProof=%s\n", __func__, tx->GetHash().ToString(), hashProofOfStake.ToString());
    }

    if (!CheckStakeKernelHash(block.nBits, pindexPrev, header, txPrev, txin.prevout, block.nTime, hashProofOfStake, gArgs.IsArgSet("-debug")))
        return error("%s: check kernel failed on coinstake %s, hashProof=%s", __func__, tx->GetHash().ToString(), hashProofOfStake.ToString()); // may occur during initial download or if behind on block chain sync

    return true;
}

// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex)
{
    assert (pindex->pprev || pindex->GetBlockHash() == Params().GetConsensus().hashGenesisBlock);
    // Hash previous checksum with flags, hashProofOfStake and nStakeModifier
    CDataStream ss(SER_GETHASH, 0);
    if (pindex->pprev)
        ss << pindex->pprev->nStakeModifierChecksum;
    ss << pindex->nFlags << pindex->hashProofOfStake << pindex->nStakeModifier;
    arith_uint256 hashChecksum = UintToArith256(Hash(ss.begin(), ss.end()));
    hashChecksum >>= (256 - 32);
    return hashChecksum.GetLow64();
}

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
    if (Params().NetworkIDString() != "main") return true; // Testnet or Regtest has no checkpoints
    if (mapStakeModifierCheckpoints.count(nHeight))
        return nStakeModifierChecksum == mapStakeModifierCheckpoints[nHeight];

    return true;
}

// Entropy bit for stake modifier if chosen by modifier
unsigned int GetStakeEntropyBit(const CBlock& block)
{
    unsigned int nEntropyBit = UintToArith256(block.GetHash()).GetLow64() & 1llu; // last bit of block hash
    LogPrint(BCLog::KERNEL, "%s: nTime=%u hashBlock=%s entropybit=%d\n", __func__, block.nTime, block.GetHash().ToString(), nEntropyBit);
    return nEntropyBit;
}
