/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _AUTHSOCKET_H
#define _AUTHSOCKET_H

#include "Common.h"
#include "Crypto/BigNumber.h"
#include "Crypto/Hash/SHA1.h"
#include "Crypto/Authentication/SRP6.h"
#include "ByteBuffer.h"
#include "IO/Networking/AsyncSocket.h"
#include "IO/Timer/TimerHandle.h"
#include "IO/Filesystem/FileHandle.h"
#include "Policies/ObjectConstructorTraits.h"
#include "AuthPackets.h"
#include <functional>

enum LockFlag
{
    NONE            = 0x00,
    IP_LOCK         = 0x01,
    FIXED_PIN       = 0x02,
    TOTP            = 0x04,
    ALWAYS_ENFORCE  = 0x08,
    GEO_COUNTRY     = 0x10,
    GEO_CITY        = 0x20
};

struct sAuthLogonProof_C;
class QueryResult;
struct RealmListLoadState;
struct GeoLockCheckState;

/// Handle login commands
class AuthSocket : public std::enable_shared_from_this<AuthSocket>, MaNGOS::Policies::NoCopyNoMove
{
    public:
        explicit AuthSocket(IO::Networking::AsyncSocket socket);
        ~AuthSocket();

        void Start();

        void DoRecvIncomingData();
        std::shared_ptr<ByteBuffer> GenerateLogonProofResponse(Crypto::Hash::SHA1::Digest const& shaDigest);
        // Phase6 (see HPHA.md "设计方案") - async now (was a synchronous PQuery-per-realm loop
        // writing directly into a caller-owned buffer). Calls `onComplete(pkt)` once every
        // realm's `numchars` lookup has finished, instead of returning the finished buffer
        // directly - see the .cpp for why a `sRealmList` iterator can't just be held across the
        // per-realm async waits.
        void LoadRealmlistAndWriteIntoBuffer(std::function<void(ByteBuffer)> onComplete);
        bool VerifyPinData(uint32 pin, PINData const& clientData);
        uint32 GenerateTotpPin(std::string const& secret, int interval);

        void _HandleLogonChallenge();
        void _HandleLogonProof();
        void _HandleLogonProof__PostRecv(std::shared_ptr<sAuthLogonProof_C const> const& lp, std::shared_ptr<PINData const> const& pinData);
        void _HandleLogonProof__PostRecv_HandleInvalidVersion(std::shared_ptr<sAuthLogonProof_C const> const& lp);
        // Phase6 (see HPHA.md "设计方案") - the tail end of a successful login (log, clear
        // brute-force counter, sessionkey UPDATE, send the final proof packet). Split out because
        // it's now reached from two places: directly (no geolock needed) and from
        // GeographicalLockCheck()'s async callback (geolock checked, not locked).
        void _HandleLogonProof__PostRecv_FinishSuccessfulLogin();
        void _HandleReconnectChallenge();
        void _HandleReconnectProof();
        void _HandleRealmList();

        //data transfer handle for patch
        void _HandleXferAccept();
        void _HandleXferResume();
        void _HandleXferCancel();

        /// Returns the IP of the peer e.g. "192.168.13.37"
        inline std::string const& GetRemoteIpString() const { return m_remoteIpAddressStringAfterProxy; }
        void CloseSocket();

    public: // A bit hacky, that this is public. In WorldSocket we have WorldSocketMgr as friend, this is not possible here
        IO::Networking::AsyncSocket m_socket;
        std::string m_remoteIpAddressStringAfterProxy; // might differ from `m_socket.m_descriptor` if behind proxy

    private:
        enum eStatus
        {
            STATUS_CHALLENGE,
            STATUS_LOGON_PROOF,
            STATUS_RECON_PROOF,
            STATUS_PATCH,
            STATUS_AUTHED,
            STATUS_INVALID,
        };

        bool VerifyVersion(uint8 const* a, int32 aLength, uint8 const* versionProof, bool isReconnect);

        // Reads the shared header/body of logon and reconnect challenges, validates sizes,
        // extracts build/os/platform/locale/login, enforces the locale allowlist, and hands
        // the parsed body to `onBody`. On any failure returns without invoking `onBody`
        // (socket closes implicitly).
        void ReadChallengeRequest(char const* logPrefix, std::function<void(std::shared_ptr<sAuthLogonChallengeBody> const&)> onBody);
        static bool IsAllowedLocale(std::string const& locale);

        // Phase6 (see HPHA.md "设计方案") - _HandleLogonChallenge()'s DB calls, chained through
        // AsyncPQuery()+EnterIoContext() instead of blocking the IO thread. Static callbacks
        // (not member-function-bound ones) so the async DB queue holds a std::shared_ptr<AuthSocket>
        // param, not a raw `this` - keeps the socket alive across the async wait even if the
        // client disconnects mid-query (see HPHA.md's UAF note). Each step only threads through
        // the state the next step actually needs; everything else it needs (m_login, srp, the
        // lock-flag/geolock members set by the previous step) is already sitting on `self` by
        // the time each step runs, same idiom this class already uses for its handshake state.
        static void _HandleLogonChallenge__OnIpBanResult(std::unique_ptr<QueryResult> result, std::shared_ptr<AuthSocket> self);
        static void _HandleLogonChallenge__OnAccountResult(std::unique_ptr<QueryResult> result, std::shared_ptr<AuthSocket> self);
        static void _HandleLogonChallenge__OnAccountBanResult(std::unique_ptr<QueryResult> result, std::shared_ptr<AuthSocket> self, uint32 pendingAccountId);
        static void _OnLoadAccountSecurityLevelsResult(std::unique_ptr<QueryResult> result, std::shared_ptr<AuthSocket> self);
        static void _HandleReconnectChallenge__OnAccountResult(std::unique_ptr<QueryResult> result, std::shared_ptr<AuthSocket> self);

        // RealmListLoadState is defined in the .cpp (needs types LoadRealmlistAndWriteIntoBuffer()
        // already pulls in there) - only forward-declared above since these two just need the
        // shared_ptr<> type to declare their signatures.
        static void _OnLoadRealmlistNumCharsResult(std::unique_ptr<QueryResult> result, std::shared_ptr<RealmListLoadState> state);
        static void _LoadRealmlistNextStep(std::shared_ptr<RealmListLoadState> state);

        // Phase6 (see HPHA.md "设计方案") - GeographicalLockCheck()'s two independent geoip
        // lookups (current IP, then previous IP), chained the same way as the RealmList batch.
        // GeoLockCheckState is defined in the .cpp, same reasoning as RealmListLoadState above.
        static void _OnGeoLockCheckCurrentIpResult(std::unique_ptr<QueryResult> result, std::shared_ptr<GeoLockCheckState> state);
        static void _OnGeoLockCheckPrevIpResult(std::unique_ptr<QueryResult> result, std::shared_ptr<GeoLockCheckState> state);

        SRP6 srp;
        BigNumber m_reconnectProof;

        bool m_promptPin = false;

        eStatus m_status = STATUS_CHALLENGE;

        std::string m_login;
        std::string m_safelogin;
        std::string m_securityInfo;
        std::string m_lastIP;
        std::string m_email;

        BigNumber m_serverSecuritySalt;
        LockFlag m_lockFlags = NONE;
        uint32 m_gridSeed = 0;
        uint32 m_geoUnlockPIN = 0;

        static constexpr uint32 Win = 0x57696E; // Win
        static constexpr uint32 OSX = 0x4F5358; // OSX

        static constexpr uint32 X86 = 0x783836; // x86
        static constexpr uint32 PPC = 0x505043; // PPC

        std::string m_os;
        std::string m_platform;
        uint32 m_accountId = 0;
        nonstd::optional<std::chrono::steady_clock::time_point> m_lastRealmListRequest;

        // Since GetLocaleByName() is _NOT_ bijective, we have to store the locale as a string. Otherwise we can't differ
        // between enUS and enGB, which is important for the patch system
        std::string m_localizationName;
        uint16 m_build = 0;

        AccountTypes GetSecurityOn(uint32 realmId) const;
        void LoadAccountSecurityLevels(uint32 accountId);
        // Phase6 (see HPHA.md "设计方案") - was a synchronous bool-returning check with two
        // blocking PQuery() calls inside; now async, calls onResult(locked) once both geoip
        // lookups are done (or synchronously, still on the caller's thread, for the early-out
        // cases that never touch the DB at all).
        static void GeographicalLockCheck(std::shared_ptr<AuthSocket> self, std::function<void(bool)> onResult);

        AccountTypes m_accountDefaultSecurityLevel = SEC_PLAYER;
        typedef std::map<uint32, AccountTypes> AccountSecurityMap;
        AccountSecurityMap m_accountSecurityOnRealm;

        // Auto kick realmd client connection after some time
        std::shared_ptr<IO::Timer::TimerHandle> m_sessionDurationTimeout = nullptr;

        // Patching stuff
        void InitAndHandOverControlToPatchHandler();
        std::unique_ptr<IO::Filesystem::FileHandleReadonly> m_pendingPatchFile = nullptr;

        void RepeatInternalXferLoop(std::shared_ptr<XFER_DATA_CHUNK> const& chunkSharedPtr);
};

#endif
