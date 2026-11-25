// Copyright (c) 2026 Airdraft
// SPDX-License-Identifier: GPL-2.0-only
#include "backend.h"

#include <fstream>
#include <ncrypt.h>
#include <wincrypt.h>

namespace airdraft {
namespace {
struct KeyHandle {
    NCRYPT_HANDLE value = 0;
    ~KeyHandle() { if (value) NCryptFreeObject(value); }
};

bool write_pem(const fs::path& file, const char* type, const BYTE* bytes, DWORD count)
{
    DWORD length = 0;
    if (!CryptBinaryToStringA(bytes, count, CRYPT_STRING_BASE64, nullptr, &length)) return false;
    std::string encoded(length, '\0');
    if (!CryptBinaryToStringA(bytes, count, CRYPT_STRING_BASE64, encoded.data(), &length)) return false;
    const auto temporary = file.wstring() + L".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << "-----BEGIN " << type << "-----\n" << encoded.c_str() << "-----END " << type << "-----\n";
        out.close();
        if (!out) return false;
    }
    return MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool is_current(const fs::path& file)
{
    if (!fs::is_regular_file(file)) return false;
    DWORD encoding = 0;
    PCCERT_CONTEXT certificate = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, file.c_str(), CERT_QUERY_CONTENT_FLAG_CERT,
            CERT_QUERY_FORMAT_FLAG_BASE64_ENCODED, 0, &encoding, nullptr, nullptr, nullptr, nullptr,
            reinterpret_cast<const void**>(&certificate))) return false;
    const bool valid = CertVerifyTimeValidity(nullptr, certificate->pCertInfo) == 0;
    CertFreeCertificateContext(certificate);
    return valid;
}
}

bool ensure_identity(const fs::path& folder)
{
    const auto cert_file = folder / L"certificate.self-signed.pem";
    const auto key_file = folder / L"key.self-signed.pem";
    if (fs::is_regular_file(key_file) && is_current(cert_file)) return true;
    fs::create_directories(folder);

    KeyHandle provider, key;
    if (NCryptOpenStorageProvider(&provider.value, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS ||
        NCryptCreatePersistedKey(provider.value, &key.value, NCRYPT_RSA_ALGORITHM, nullptr, 0, 0) != ERROR_SUCCESS)
        return false;
    DWORD bits = 2048, policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
    if (NCryptSetProperty(key.value, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<BYTE*>(&bits), sizeof(bits), 0) != ERROR_SUCCESS ||
        NCryptSetProperty(key.value, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<BYTE*>(&policy), sizeof(policy), 0) != ERROR_SUCCESS ||
        NCryptFinalizeKey(key.value, 0) != ERROR_SUCCESS) return false;

    DWORD subject_size = 0;
    constexpr auto name = L"CN=AirDrop receiver";
    if (!CertStrToNameW(X509_ASN_ENCODING, name, CERT_X500_NAME_STR, nullptr, nullptr, &subject_size, nullptr)) return false;
    std::vector<BYTE> subject(subject_size);
    if (!CertStrToNameW(X509_ASN_ENCODING, name, CERT_X500_NAME_STR, nullptr, subject.data(), &subject_size, nullptr)) return false;
    CERT_NAME_BLOB blob{subject_size, subject.data()};
    CRYPT_ALGORITHM_IDENTIFIER algorithm{const_cast<char*>(szOID_RSA_SHA256RSA), {}};
    SYSTEMTIME expiry{};
    GetSystemTime(&expiry);
    expiry.wYear++;
    if (expiry.wMonth == 2 && expiry.wDay == 29) expiry.wDay = 28;
    const auto certificate = CertCreateSelfSignCertificate(key.value, &blob, CERT_CREATE_SELFSIGN_NO_KEY_INFO,
        nullptr, &algorithm, nullptr, &expiry, nullptr);
    if (!certificate) return false;
    DWORD key_size = 0;
    auto result = NCryptExportKey(key.value, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, nullptr, 0, &key_size, 0);
    std::vector<BYTE> pkcs8(key_size);
    if (result == ERROR_SUCCESS)
        result = NCryptExportKey(key.value, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, pkcs8.data(), key_size, &key_size, 0);
    const bool saved = result == ERROR_SUCCESS && write_pem(key_file, "PRIVATE KEY", pkcs8.data(), key_size)
        && write_pem(cert_file, "CERTIFICATE", certificate->pbCertEncoded, certificate->cbCertEncoded);
    if (!pkcs8.empty()) SecureZeroMemory(pkcs8.data(), pkcs8.size());
    CertFreeCertificateContext(certificate);
    return saved;
}
}
