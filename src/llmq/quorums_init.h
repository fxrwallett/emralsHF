// Copyright (c) 2018-2019 The Dash Core developers
// Copyright (c) 2019 The Bit Green Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef EMRALS_LLMQ_QUORUMS_INIT_H
#define EMRALS_LLMQ_QUORUMS_INIT_H

class CDBWrapper;
class CSpecialDB;
class CScheduler;

namespace llmq
{

// If true, we will connect to all new quorums and watch their communication
static const bool DEFAULT_WATCH_QUORUMS = false;

// Init/destroy LLMQ globals
void InitLLMQSystem(CSpecialDB& specialDb, CScheduler* scheduler, bool unitTests, bool fWipe = false);
void DestroyLLMQSystem();

// Manage scheduled tasks, threads, listeners etc.
void StartLLMQSystem();
void StopLLMQSystem();
void InterruptLLMQSystem();
}

#endif //EMRALS_LLMQ_QUORUMS_INIT_H
