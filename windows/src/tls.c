#include "link.h"
static CredHandle client_cred, server_cred;
static int client_ok, server_ok;
static PCCERT_CONTEXT certificate;
static HCRYPTPROV provider;
static HCERTSTORE issuer_store;
struct retired_credential
{
    CredHandle handle;
    struct retired_credential *next;
};
static struct retired_credential *retired_credentials;
static CRITICAL_SECTION credential_lock;
static int credential_lock_ok;
static struct tls *connections;

static int certificate_matches_key(HCRYPTPROV crypto, PCCERT_CONTEXT cert)
{
    PCERT_PUBLIC_KEY_INFO public_info = NULL;
    DWORD public_len = 0;
    int ok = 0;
    if (cert && CryptExportPublicKeyInfo(crypto, AT_KEYEXCHANGE, X509_ASN_ENCODING, NULL,
                                         &public_len))
    {
        public_info = (PCERT_PUBLIC_KEY_INFO)malloc(public_len);
        if (public_info && CryptExportPublicKeyInfo(crypto, AT_KEYEXCHANGE, X509_ASN_ENCODING,
                                                    public_info, &public_len))
        {
            ok = CertComparePublicKeyInfo(X509_ASN_ENCODING,
                                          &cert->pCertInfo->SubjectPublicKeyInfo, public_info);
        }
    }
    free(public_info);
    return ok;
}

static SECURITY_STATUS acquire_server_credential(CredHandle *handle)
{
    SCHANNEL_CRED cred;
    TimeStamp expiry;
    memset(&cred, 0, sizeof(cred));
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.cCreds = 1;
    cred.paCred = &certificate;
    cred.hRootStore = issuer_store;
    cred.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER | SCH_CRED_DISABLE_RECONNECTS;
    cred.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
    return AcquireCredentialsHandleW(NULL, UNISP_NAME_W, SECPKG_CRED_INBOUND, NULL, &cred, NULL,
                                     NULL, handle, &expiry);
}
static int send_all(SOCKET s, const void *data, int len)
{
    const char *p = (const char *)data;
    int n;
    while (len > 0)
    {
        n = send(s, p, len, 0);
        if (n <= 0)
        {
            return 0;
        }
        p += n;
        len -= n;
    }
    return 1;
}
int tls_init(void)
{
    WCHAR path[MAX_PATH], container[96], name[] = L"CN=ALTEK Link device";
    unsigned char *der = NULL;
    DWORD len = 0, n = 0;
    HCRYPTKEY key = 0;
    CERT_NAME_BLOB subject;
    CRYPT_KEY_PROV_INFO info;
    CRYPT_ALGORITHM_IDENTIFIER alg;
    SYSTEMTIME from, to;
    SCHANNEL_CRED cred;
    TimeStamp expiry;
    SECURITY_STATUS status;
    char ident[40];
    int certificate_changed = 0, from_stable = 0;
    WCHAR stable[MAX_PATH], pd[MAX_PATH], spath[MAX_PATH];
    InitializeCriticalSection(&credential_lock);
    credential_lock_ok = 1;
    log_event("TLS_INIT", "begin");
    path_join(path, app.dir, L"settings.ini");
    /* The RSA keyset lives in the user profile key store (survives uninstall),
     * but its container name and identity.der used to live in %LOCALAPPDATA%,
     * which an uninstall wipes -> a brand-new device ID every reinstall.
     * Keep both in %PROGRAMDATA%\AltekLink so the identity survives. */
    if (GetEnvironmentVariableW(L"PROGRAMDATA", pd, MAX_PATH) > 0 && pd[0])
    {
        path_join(stable, pd, L"AltekLink");
        CreateDirectoryW(stable, NULL);
    }
    else
    {
        lstrcpynW(stable, app.dir, MAX_PATH);
    }
    GetPrivateProfileStringW(L"identity", L"container", L"", container, 96, path);
    if (!container[0])
    {
        HANDLE f;
        path_join(spath, stable, L"container.txt");
        f = CreateFileW(spath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE)
        {
            DWORD got = 0;
            ReadFile(f, container, 95 * sizeof(WCHAR), &got, NULL);
            container[got / sizeof(WCHAR)] = 0;
            CloseHandle(f);
        }
    }
    if (!container[0])
    {
        /* Derive the key-container name from the machine GUID so the same keyset
         * is located on every reinstall instead of generating a random name. */
        HKEY hk;
        WCHAR mg[96];
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0,
                          KEY_READ, &hk) == ERROR_SUCCESS)
        {
            DWORD t = REG_SZ, s = sizeof(mg);
            if (RegQueryValueExW(hk, L"MachineGuid", NULL, &t, (LPBYTE)mg, &s) == ERROR_SUCCESS &&
                mg[0])
            {
                mg[8] = 0;
                _snwprintf(container, 96, L"ALTEK-Link-%S", mg);
            }
            RegCloseKey(hk);
        }
        if (!container[0])
        {
            uid(ident);
            _snwprintf(container, 96, L"ALTEK-Link-%S", ident);
        }
        path_join(spath, stable, L"container.txt");
        {
            HANDLE f = CreateFileW(spath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (f != INVALID_HANDLE_VALUE)
            {
                DWORD put = 0;
                WriteFile(f, container, (DWORD)(wcslen(container) * sizeof(WCHAR)), &put, NULL);
                CloseHandle(f);
            }
        }
    }
    /* The live copy remains in the user-writable data directory. ProgramData is
       only a best-effort reinstall backup because ordinary users may not have
       write permission there. */
    path_join(spath, stable, L"identity.der");
    path_join(path, app.dir, L"identity.der");
    if (!CryptAcquireContextW(&provider, container, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, 0))
    {
        if (GetLastError() != (DWORD)NTE_BAD_KEYSET ||
            !CryptAcquireContextW(&provider, container, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                                  CRYPT_NEWKEYSET))
        {
            goto fail;
        }
    }
    if (!CryptGetUserKey(provider, AT_KEYEXCHANGE, &key))
    {
        if (!CryptGenKey(provider, AT_KEYEXCHANGE, 2048 << 16, &key))
        {
            goto fail;
        }
    }
    CryptDestroyKey(key);
    key = 0;
    if (read_file(path, &der, &len, 16384))
    {
        certificate = CertCreateCertificateContext(X509_ASN_ENCODING, der, len);
        free(der);
        der = NULL;
    }
    if (certificate && !certificate_matches_key(provider, certificate))
    {
        log_event("IDENTITY_REPAIR", "local certificate/key mismatch");
        CertFreeCertificateContext(certificate);
        certificate = NULL;
    }
    if (!certificate && read_file(spath, &der, &len, 16384))
    {
        certificate = CertCreateCertificateContext(X509_ASN_ENCODING, der, len);
        free(der);
        der = NULL;
        from_stable = certificate != NULL;
    }
    if (certificate && !certificate_matches_key(provider, certificate))
    {
        log_event("IDENTITY_REPAIR", "backup certificate/key mismatch");
        CertFreeCertificateContext(certificate);
        certificate = NULL;
        from_stable = 0;
    }
    if (!certificate)
    {
        memset(&subject, 0, sizeof(subject));
        if (!CertStrToNameW(X509_ASN_ENCODING, name, CERT_X500_NAME_STR, NULL, NULL, &n, NULL))
        {
            goto fail;
        }
        subject.pbData = (BYTE *)malloc(n);
        subject.cbData = n;
        if (!subject.pbData)
        {
            goto fail;
        }
        if (!CertStrToNameW(X509_ASN_ENCODING, name, CERT_X500_NAME_STR, NULL, subject.pbData, &n,
                            NULL))
        {
            free(subject.pbData);
            goto fail;
        }
        memset(&info, 0, sizeof(info));
        info.pwszContainerName = container;
        info.pwszProvName = MS_ENH_RSA_AES_PROV_W;
        info.dwProvType = PROV_RSA_AES;
        info.dwKeySpec = AT_KEYEXCHANGE;
        memset(&alg, 0, sizeof(alg));
        alg.pszObjId = szOID_RSA_SHA256RSA;
        memset(&from, 0, sizeof(from));
        from.wYear = 2020;
        from.wMonth = 1;
        from.wDay = 1;
        to = from;
        to.wYear = 2049;
        to.wMonth = 12;
        to.wDay = 31;
        certificate =
            CertCreateSelfSignCertificate(provider, &subject, 0, &info, &alg, &from, &to, NULL);
        free(subject.pbData);
        certificate_changed = 1;
        if (!certificate)
        {
            goto fail;
        }
    }
    if ((certificate_changed || from_stable) &&
        !atomic_write(path, certificate->pbCertEncoded, certificate->cbCertEncoded))
    {
        goto fail;
    }
    if (certificate_changed)
    {
        (void)atomic_write(spath, certificate->pbCertEncoded, certificate->cbCertEncoded);
    }
    if (!certificate)
    {
        goto fail;
    }
    memset(&info, 0, sizeof(info));
    info.pwszContainerName = container;
    info.pwszProvName = MS_ENH_RSA_AES_PROV_W;
    info.dwProvType = PROV_RSA_AES;
    info.dwKeySpec = AT_KEYEXCHANGE;
    if (!CertSetCertificateContextProperty(certificate, CERT_KEY_PROV_INFO_PROP_ID, 0, &info))
    {
        goto fail;
    }
    if (!digest(certificate->pbCertEncoded, certificate->cbCertEncoded, app.id))
    {
        goto fail;
    }
    memset(&cred, 0, sizeof(cred));
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.cCreds = 1;
    cred.paCred = &certificate;
    cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    cred.dwFlags =
        SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_NO_SYSTEM_MAPPER;
    status = AcquireCredentialsHandleW(NULL, UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, &cred, NULL,
                                       NULL, &client_cred, &expiry);
    if (status != SEC_E_OK)
    {
        log_event("TLS_CREDENTIAL", "client=0x%08lx", status);
        goto fail;
    }
    client_ok = 1;
    log_event("TLS_INIT", "client credential ready");
    /* This store contains trusted client identities, never the server's own
       leaf. All ALTEK identities intentionally use the same subject name, so
       adding the local leaf beside a peer leaf makes Schannel select an
       ambiguous self-signed issuer during client authentication. */
    issuer_store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    if (!issuer_store)
    {
        goto fail;
    }
    status = acquire_server_credential(&server_cred);
    if (status != SEC_E_OK)
    {
        log_event("TLS_CREDENTIAL", "server=0x%08lx", status);
        goto fail;
    }
    server_ok = 1;
    log_event("TLS_INIT", "server credential ready");
    return 1;
fail:
    log_event("IDENTITY_ERROR", "win32=%lu", GetLastError());
    tls_cleanup();
    return 0;
}
void tls_cleanup(void)
{
    struct retired_credential *old;
    if (client_ok)
    {
        FreeCredentialsHandle(&client_cred);
    }
    if (server_ok)
    {
        FreeCredentialsHandle(&server_cred);
    }
    while (retired_credentials)
    {
        old = retired_credentials;
        retired_credentials = old->next;
        FreeCredentialsHandle(&old->handle);
        free(old);
    }
    if (issuer_store)
    {
        CertCloseStore(issuer_store, 0);
        issuer_store = NULL;
    }
    if (certificate)
    {
        CertFreeCertificateContext(certificate);
    }
    if (provider)
    {
        CryptReleaseContext(provider, 0);
    }
    client_ok = server_ok = 0;
    certificate = NULL;
    provider = 0;
    if (credential_lock_ok)
    {
        DeleteCriticalSection(&credential_lock);
        credential_lock_ok = 0;
    }
}
int tls_identity_certificate(char *out, int cap)
{
    DWORD size;
    if (!out || cap < 1 || !certificate)
    {
        return 0;
    }
    size = (DWORD)cap;
    if (!CryptBinaryToStringA(certificate->pbCertEncoded, certificate->cbCertEncoded,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &size))
    {
        out[0] = 0;
        return 0;
    }
    return 1;
}
int tls_trust_discovery_certificate(const char *encoded, const char *expected)
{
    unsigned char der[2048];
    DWORD len = sizeof(der);
    PCCERT_CONTEXT peer = NULL;
    PCCERT_CONTEXT existing = NULL;
    CredHandle replacement;
    SECURITY_STATUS status;
    struct retired_credential *old = NULL;
    char id[65];
    int ok = 0;
    if (!issuer_store || !encoded || !encoded[0] || !valid_hex(expected, 64) ||
        !strcmp(expected, app.id) ||
        !CryptStringToBinaryA(encoded, 0, CRYPT_STRING_BASE64, der, &len, NULL, NULL))
    {
        return 0;
    }
    peer = CertCreateCertificateContext(X509_ASN_ENCODING, der, len);
    if (peer && peer->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId &&
        !strcmp(peer->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId, szOID_RSA_RSA) &&
        CertGetPublicKeyLength(X509_ASN_ENCODING, &peer->pCertInfo->SubjectPublicKeyInfo) >= 2048 &&
        CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING,
                                          CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)peer,
                                          CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void *)peer, 0,
                                          NULL) &&
        digest(peer->pbCertEncoded, peer->cbCertEncoded, id) && !strcmp(id, expected) &&
        strcmp(id, app.id))
    {
        EnterCriticalSection(&credential_lock);
        existing = CertFindCertificateInStore(issuer_store, X509_ASN_ENCODING, 0,
                                               CERT_FIND_EXISTING, peer, NULL);
        if (existing)
        {
            CertFreeCertificateContext(existing);
            ok = 1;
        }
        else if (CertAddCertificateContextToStore(issuer_store, peer, CERT_STORE_ADD_NEW, NULL))
        {
            status = acquire_server_credential(&replacement);
            if (status == SEC_E_OK)
            {
                old = (struct retired_credential *)malloc(sizeof(*old));
                if (old)
                {
                    old->handle = server_cred;
                    old->next = retired_credentials;
                    retired_credentials = old;
                    server_cred = replacement;
                    ok = 1;
                    log_event("TLS_TRUST", "peer=%.12s credential=refreshed", id);
                }
                else
                {
                    FreeCredentialsHandle(&replacement);
                }
            }
            else
            {
                log_event("TLS_CREDENTIAL", "refresh=0x%08lx peer=%.12s", status, id);
            }
        }
        LeaveCriticalSection(&credential_lock);
    }
    if (!ok)
    {
        log_event("TLS_TRUST", "peer=%.12s rejected", expected ? expected : "");
    }
    if (peer)
    {
        CertFreeCertificateContext(peer);
    }
    return ok;
}
static int verify_peer(struct tls *t, const char *expected)
{
    PCCERT_CONTEXT cert = NULL;
    int ok = 0;
    if (QueryContextAttributesW(&t->context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK)
    {
        return 0;
    }
    if (cert->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId &&
        !strcmp(cert->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId, szOID_RSA_RSA) &&
        CertGetPublicKeyLength(X509_ASN_ENCODING, &cert->pCertInfo->SubjectPublicKeyInfo) >= 2048 &&
        CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT,
                                          (void *)cert, CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT,
                                          (void *)cert, 0, NULL) &&
        digest(cert->pbCertEncoded, cert->cbCertEncoded, t->peer))
    {
        ok = (!expected || !expected[0] || !strcmp(expected, t->peer)) && strcmp(t->peer, app.id);
    }
    CertFreeCertificateContext(cert);
    if (!ok)
    {
        log_event("TLS_IDENTITY", "certificate rejected / fingerprint mismatch");
    }
    return ok;
}
static struct tls *handshake(SOCKET socket, int server, const char *expected)
{
    struct tls *t = (struct tls *)calloc(1, sizeof(*t));
    SecBuffer ib[2], ob;
    SecBufferDesc input, output;
    DWORD attrs = 0, flags;
    TimeStamp expiry;
    SECURITY_STATUS status = SEC_I_CONTINUE_NEEDED;
    CredHandle accept_cred;
    int first = 1, need_read = server, n, step = 0;
    const char *where = "start";
    DWORD timeout = 12000;
    BOOL yes = TRUE;
    if (!t)
    {
        closesocket(socket);
        return NULL;
    }
    t->socket = socket;
    InitializeCriticalSection(&t->write_lock);
    EnterCriticalSection(&app.lock);
    t->next = connections;
    connections = t;
    LeaveCriticalSection(&app.lock);
    if (!app.running)
    {
        tls_close(t);
        return NULL;
    }
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
    if (server)
    {
        EnterCriticalSection(&credential_lock);
        accept_cred = server_cred;
        LeaveCriticalSection(&credential_lock);
    }
    flags = server ? (ASC_REQ_STREAM | ASC_REQ_CONFIDENTIALITY | ASC_REQ_ALLOCATE_MEMORY |
                      ASC_REQ_MUTUAL_AUTH)
                   : (ISC_REQ_STREAM | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                      ISC_REQ_MANUAL_CRED_VALIDATION | ISC_REQ_USE_SUPPLIED_CREDS);
    for (;;)
    {
        if (need_read)
        {
            where = "recv";
            if (t->enc_len == (int)sizeof(t->encrypted))
            {
                goto fail;
            }
            n = recv(socket, (char *)t->encrypted + t->enc_len, sizeof(t->encrypted) - t->enc_len,
                     0);
            if (n <= 0)
            {
                goto fail;
            }
            t->enc_len += n;
        }
        memset(ib, 0, sizeof(ib));
        ib[0].BufferType = SECBUFFER_TOKEN;
        ib[0].pvBuffer = t->encrypted;
        ib[0].cbBuffer = t->enc_len;
        ib[1].BufferType = SECBUFFER_EMPTY;
        input.ulVersion = SECBUFFER_VERSION;
        input.cBuffers = 2;
        input.pBuffers = ib;
        memset(&ob, 0, sizeof(ob));
        ob.BufferType = SECBUFFER_TOKEN;
        output.ulVersion = SECBUFFER_VERSION;
        output.cBuffers = 1;
        output.pBuffers = &ob;
        step++;
        where = "sspi";
        if (server)
        {
            status =
                AcceptSecurityContext(&accept_cred, first ? NULL : &t->context, &input, flags,
                                      SECURITY_NATIVE_DREP, &t->context, &output, &attrs, &expiry);
        }
        else
        {
            status = InitializeSecurityContextW(
                &client_cred, first ? NULL : &t->context, NULL, flags, 0, SECURITY_NATIVE_DREP,
                first ? NULL : &input, 0, &t->context, &output, &attrs, &expiry);
        }
        if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED)
        {
            t->context_ok = 1;
        }
        if (ob.pvBuffer)
        {
            int sent = 1;
            if (ob.cbBuffer)
            {
                sent = send_all(socket, ob.pvBuffer, ob.cbBuffer);
            }
            FreeContextBuffer(ob.pvBuffer);
            if (!sent)
            {
                where = "send";
                goto fail;
            }
        }
        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            need_read = 1;
            continue;
        }
        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
        {
            goto fail;
        }
        first = 0;
        if (ib[1].BufferType == SECBUFFER_EXTRA)
        {
            memmove(t->encrypted, t->encrypted + t->enc_len - ib[1].cbBuffer, ib[1].cbBuffer);
            t->enc_len = ib[1].cbBuffer;
        }
        else
        {
            t->enc_len = 0;
        }
        if (status == SEC_E_OK)
        {
            break;
        }
        need_read = t->enc_len == 0;
    }
    if (!verify_peer(t, expected) ||
        QueryContextAttributesW(&t->context, SECPKG_ATTR_STREAM_SIZES, &t->sizes) != SEC_E_OK)
    {
        goto fail;
    }
    return t;
fail:
    log_event("TLS_HANDSHAKE", "role=%s step=%d where=%s status=0x%08lx input=%d socket=%d",
              server ? "server" : "client", step, where, status, t->enc_len, WSAGetLastError());
    tls_close(t);
    return NULL;
}
struct tls *tls_accept(SOCKET socket)
{
    return handshake(socket, 1, NULL);
}
struct tls *tls_connect(const char *ip, const char *expected)
{
    SOCKET s;
    struct sockaddr_in a;
    u_long nonblock = 1;
    fd_set fds;
    struct timeval tv;
    int err = 0, len = sizeof(err);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
    {
        return NULL;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = inet_addr(ip);
    ioctlsocket(s, FIONBIO, &nonblock);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == SOCKET_ERROR)
    {
        if (WSAGetLastError() != WSAEWOULDBLOCK)
        {
            closesocket(s);
            return NULL;
        }
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        tv.tv_sec = 4;
        tv.tv_usec = 0;
        if (select(0, NULL, &fds, NULL, &tv) <= 0 ||
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) || err)
        {
            closesocket(s);
            return NULL;
        }
    }
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    return handshake(s, 0, expected);
}
void tls_close(struct tls *t)
{
    struct tls **p;
    if (!t)
    {
        return;
    }
    EnterCriticalSection(&app.lock);
    for (p = &connections; *p; p = &(*p)->next)
    {
        if (*p == t)
        {
            *p = t->next;
            break;
        }
    }
    shutdown(t->socket, SD_BOTH);
    closesocket(t->socket);
    LeaveCriticalSection(&app.lock);
    if (t->context_ok)
    {
        DeleteSecurityContext(&t->context);
    }
    DeleteCriticalSection(&t->write_lock);
    free(t);
}
int tls_read(struct tls *t, void *data, int len)
{
    unsigned char *out = (unsigned char *)data;
    int n, i, need = 0;
    SecBuffer b[4];
    SecBufferDesc d;
    SECURITY_STATUS status;
    while (len > 0)
    {
        if (t->plain_pos < t->plain_len)
        {
            n = t->plain_len - t->plain_pos;
            if (n > len)
            {
                n = len;
            }
            memcpy(out, t->plain + t->plain_pos, n);
            t->plain_pos += n;
            out += n;
            len -= n;
            continue;
        }
        if (!t->enc_len || need)
        {
            if (t->enc_len == (int)sizeof(t->encrypted))
            {
                return 0;
            }
            n = recv(t->socket, (char *)t->encrypted + t->enc_len,
                     sizeof(t->encrypted) - t->enc_len, 0);
            if (n <= 0)
            {
                return 0;
            }
            t->enc_len += n;
        }
        memset(b, 0, sizeof(b));
        b[0].BufferType = SECBUFFER_DATA;
        b[0].pvBuffer = t->encrypted;
        b[0].cbBuffer = t->enc_len;
        d.ulVersion = SECBUFFER_VERSION;
        d.cBuffers = 4;
        d.pBuffers = b;
        status = DecryptMessage(&t->context, &d, 0, NULL);
        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            need = 1;
            continue;
        }
        if (status != SEC_E_OK)
        {
            return 0;
        }
        need = 0;
        t->plain_pos = t->plain_len = 0;
        for (i = 0; i < 4; i++)
        {
            if (b[i].BufferType == SECBUFFER_DATA)
            {
                if (b[i].cbBuffer > sizeof(t->plain))
                {
                    return 0;
                }
                memcpy(t->plain, b[i].pvBuffer, b[i].cbBuffer);
                t->plain_len = b[i].cbBuffer;
            }
        }
        n = 0;
        for (i = 0; i < 4; i++)
        {
            if (b[i].BufferType == SECBUFFER_EXTRA)
            {
                n = b[i].cbBuffer;
                memmove(t->encrypted, t->encrypted + t->enc_len - n, n);
                break;
            }
        }
        t->enc_len = n;
    }
    return 1;
}
static int write_locked(struct tls *t, const void *data, int len)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned char *buf;
    SecBuffer b[4];
    SecBufferDesc d;
    int n, ok = 1, total;
    buf =
        (unsigned char *)malloc(t->sizes.cbHeader + t->sizes.cbMaximumMessage + t->sizes.cbTrailer);
    if (!buf)
    {
        return 0;
    }
    while (len > 0)
    {
        n = len;
        if (n > (int)t->sizes.cbMaximumMessage)
        {
            n = t->sizes.cbMaximumMessage;
        }
        memset(b, 0, sizeof(b));
        b[0].BufferType = SECBUFFER_STREAM_HEADER;
        b[0].pvBuffer = buf;
        b[0].cbBuffer = t->sizes.cbHeader;
        b[1].BufferType = SECBUFFER_DATA;
        b[1].pvBuffer = buf + t->sizes.cbHeader;
        b[1].cbBuffer = n;
        memcpy(b[1].pvBuffer, p, n);
        b[2].BufferType = SECBUFFER_STREAM_TRAILER;
        b[2].pvBuffer = buf + t->sizes.cbHeader + n;
        b[2].cbBuffer = t->sizes.cbTrailer;
        d.ulVersion = SECBUFFER_VERSION;
        d.cBuffers = 4;
        d.pBuffers = b;
        if (EncryptMessage(&t->context, 0, &d, 0) != SEC_E_OK)
        {
            ok = 0;
            break;
        }
        total = b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer;
        if (!send_all(t->socket, buf, total))
        {
            ok = 0;
            break;
        }
        len -= n;
        p += n;
    }
    free(buf);
    return ok;
}
int tls_write(struct tls *t, const void *data, int len)
{
    int ok;
    EnterCriticalSection(&t->write_lock);
    ok = write_locked(t, data, len);
    LeaveCriticalSection(&t->write_lock);
    return ok;
}
int wire_send(struct tls *t, const void *data, int len)
{
    unsigned char h[4];
    int ok;
    if (len < 1 || len > MAX_MEDIA)
    {
        return 0;
    }
    put32(h, len);
    EnterCriticalSection(&t->write_lock);
    ok = write_locked(t, h, 4) && write_locked(t, data, len);
    LeaveCriticalSection(&t->write_lock);
    return ok;
}
int wire_json(struct tls *t, const char *s)
{
    return strlen(s) <= MAX_JSON && wire_send(t, s, (int)strlen(s));
}
unsigned char *wire_read(struct tls *t, int *len, int max)
{
    unsigned char h[4], *b;
    unsigned long n;
    if (!tls_read(t, h, 4))
    {
        return NULL;
    }
    n = be32(h);
    if (!n || n > (unsigned long)max)
    {
        return NULL;
    }
    b = (unsigned char *)malloc(n + 1);
    if (!b)
    {
        return NULL;
    }
    if (!tls_read(t, b, n))
    {
        free(b);
        return NULL;
    }
    b[n] = 0;
    *len = n;
    return b;
}

void tls_shutdown_all(void)
{
    struct tls *t;
    EnterCriticalSection(&app.lock);
    for (t = connections; t; t = t->next)
    {
        shutdown(t->socket, SD_BOTH);
    }
    LeaveCriticalSection(&app.lock);
}
