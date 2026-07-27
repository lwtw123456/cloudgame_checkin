#define CPPHTTPLIB_OPENSSL_SUPPORT 
#include "httplib.h"
#include <nlohmann/json.hpp>

#include "offset_estimator.hpp"

#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <ctime>
#include <fstream>
#include <thread>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>
#include <tuple>
#include <cstdlib>
#include <cstdio>

namespace fs = std::filesystem;

using json = nlohmann::json;

namespace logger {

enum Level { DEBUG=0, INFO, WARN, ERROR_ };
static Level g_level = INFO;

static std::string now_str() {
    auto t  = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return os.str();
}
static const char* level_str(Level l) {
    switch(l){ case DEBUG: return "DEBUG"; case INFO: return "INFO"; case WARN: return "WARN"; default: return "ERROR"; }
}

template<typename... Args>
static void log(Level l, const std::string& fmt, Args&&... args) {
    if (l < g_level) return;
    std::ostringstream os;
    const char* p = fmt.c_str();
    std::vector<std::string> strs;
    auto push = [&](auto&& v){
        std::ostringstream tmp; tmp << v; strs.push_back(tmp.str());
    };
    (push(std::forward<Args>(args)), ...);
    size_t idx = 0;
    while (*p) {
        if (*p == '{' && *(p+1) == '}') {
            if (idx < strs.size()) os << strs[idx++];
            p += 2;
        } else {
            os << *p++;
        }
    }
    std::cerr << now_str() << " [" << level_str(l) << "] " << os.str() << "\n";
}

#define LOG_INFO(fmt,  ...) logger::log(logger::INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt,  ...) logger::log(logger::WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) logger::log(logger::ERROR_,fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) logger::log(logger::DEBUG, fmt, ##__VA_ARGS__)
} // namespace logger

namespace util {

static std::mt19937_64& rng() {
    static std::mt19937_64 g(std::random_device{}());
    return g;
}

static std::string random_hex(size_t len) {
    const char* h = "0123456789abcdef";
    std::string r; r.reserve(len);
    std::uniform_int_distribution<int> d(0,15);
    for (size_t i=0;i<len;++i) r += h[d(rng())];
    return r;
}

static std::string new_uuid_v4() {
    unsigned char buf[16];
    RAND_bytes(buf, 16);
    buf[6] = (buf[6] & 0x0F) | 0x40;
    buf[8] = (buf[8] & 0x3F) | 0x80;
    std::ostringstream os;
    for (int i=0;i<16;++i) {
        if (i==4||i==6||i==8||i==10) os << '-';
        os << std::hex << std::setw(2) << std::setfill('0') << (int)buf[i];
    }
    return os.str();
}

static long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
static long long now_s() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

static std::string random_choice(std::initializer_list<std::string> lst) {
    std::uniform_int_distribution<size_t> d(0, lst.size()-1);
    return *(lst.begin() + d(rng()));
}

static std::string url_encode(const std::string& s, const std::string& safe = "") {
    std::ostringstream os;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~'
            || safe.find(c) != std::string::npos) {
            os << c;
        } else {
            os << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        }
    }
    return os.str();
}

static std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  dlen = 0;
    HMAC(EVP_sha256(),
         key.data(),  (int)key.size(),
         (const unsigned char*)data.data(), data.size(),
         digest, &dlen);
    std::ostringstream os;
    for (unsigned int i=0;i<dlen;++i)
        os << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return os.str();
}

static std::string fetch_cloudgame_version() {
    httplib::SSLClient cli("webstatic.mihoyo.com", 443);
    cli.set_follow_location(true);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(15, 0);

    auto res = cli.Get("/common/clgm-web-app/ys/VERSION.txt");
    if (!res) {
        throw std::runtime_error(
            std::string("fetch_cloudgame_version failed: ")
            + httplib::to_string(res.error()));
    }
    if (res->status != 200) {
        throw std::runtime_error(
            "fetch_cloudgame_version HTTP " + std::to_string(res->status));
    }

    std::string ver = res->body;
    ver.erase(0, ver.find_first_not_of(" \t\r\n"));
    ver.erase(ver.find_last_not_of(" \t\r\n") + 1);
    return ver;
}

} // namespace util

class HttpSession; // forward declaration

// ==================== Geetest / Aigis ====================
namespace geetest {
static std::string json_scalar_to_string(
    const json& obj,
    const char* key,
    const std::string& fallback = "")
{
    auto it = obj.find(key);

    if (it == obj.end() || it->is_null()) {
        return fallback;
    }

    if (it->is_string()) {
        return it->get<std::string>();
    }

    if (it->is_number() || it->is_boolean()) {
        return it->dump();
    }

    return fallback;
}

static std::string aes_cbc_encrypt_hex(const std::string& plaintext,
                                        const std::string& key) {
    const unsigned char iv[16] = {
        '0','0','0','0','0','0','0','0',
        '0','0','0','0','0','0','0','0'
    };
    size_t block = 16;
    size_t pad_len = block - (plaintext.size() % block);
    std::string padded = plaintext + std::string(pad_len, (char)pad_len);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>
        ctx_guard(ctx, EVP_CIPHER_CTX_free);

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
            (const unsigned char*)key.data(), iv) != 1)
        throw std::runtime_error("EVP_EncryptInit_ex failed");

    EVP_CIPHER_CTX_set_padding(ctx, 0);

    std::vector<unsigned char> out(padded.size() + block);
    int out_len1 = 0, out_len2 = 0;
    if (EVP_EncryptUpdate(ctx, out.data(), &out_len1,
            (const unsigned char*)padded.data(), (int)padded.size()) != 1)
        throw std::runtime_error("EVP_EncryptUpdate failed");
    if (EVP_EncryptFinal_ex(ctx, out.data() + out_len1, &out_len2) != 1)
        throw std::runtime_error("EVP_EncryptFinal_ex failed");

    std::ostringstream os;
    for (int i = 0; i < out_len1 + out_len2; ++i)
        os << std::hex << std::setw(2) << std::setfill('0') << (int)out[i];
    return os.str();
}

static std::string gt_rsa_encrypt_hex(const std::string& plaintext) {
    static const char* N_HEX =
        "00C1E3934D1614465B33053E7F48EE4EC87B14B95EF88947713D25EECBFF7E74C7"
        "977D02DC1D9451F79DD5D1C10C29ACB6A9B4D6FB7D0A0279B6719E1772565F09AF"
        "627715919221AEF91899CAE08C0D686D748B20A3603BE2318CA6BC2B59706592A9"
        "219D0BF05C9F65023A21D2330807252AE0066D59CEEFA5F2748EA80BAB81";

    BIGNUM* n = nullptr;
    BN_hex2bn(&n, N_HEX);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> n_guard(n, BN_free);

    BIGNUM* e_bn = BN_new();
    std::unique_ptr<BIGNUM, decltype(&BN_free)> e_guard(e_bn, BN_free);
    BN_set_word(e_bn, 0x10001);

    EVP_PKEY* pkey = nullptr;
    {
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
        if (!pctx) throw std::runtime_error("EVP_PKEY_CTX_new_from_name failed");
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>
            pctx_guard(pctx, EVP_PKEY_CTX_free);

        OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
        if (!bld) throw std::runtime_error("OSSL_PARAM_BLD_new failed");
        std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)>
            bld_guard(bld, OSSL_PARAM_BLD_free);

        if (!OSSL_PARAM_BLD_push_BN(bld, "n", n))
            throw std::runtime_error("push n failed");
        if (!OSSL_PARAM_BLD_push_BN(bld, "e", e_bn))
            throw std::runtime_error("push e failed");

        OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
        if (!params) throw std::runtime_error("OSSL_PARAM_BLD_to_param failed");
        std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)>
            params_guard(params, OSSL_PARAM_free);

        if (EVP_PKEY_fromdata_init(pctx) <= 0)
            throw std::runtime_error("EVP_PKEY_fromdata_init failed");
        if (EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0)
            throw std::runtime_error("EVP_PKEY_fromdata failed");
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey_guard(pkey, EVP_PKEY_free);

    EVP_PKEY_CTX* ectx = EVP_PKEY_CTX_new(pkey, nullptr);
    if (!ectx) throw std::runtime_error("EVP_PKEY_CTX_new failed");
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>
        ectx_guard(ectx, EVP_PKEY_CTX_free);

    if (EVP_PKEY_encrypt_init(ectx) <= 0)
        throw std::runtime_error("EVP_PKEY_encrypt_init failed");
    if (EVP_PKEY_CTX_set_rsa_padding(ectx, RSA_PKCS1_PADDING) <= 0)
        throw std::runtime_error("set_rsa_padding failed");

    const unsigned char* in = (const unsigned char*)plaintext.data();
    size_t outlen = 0;
    if (EVP_PKEY_encrypt(ectx, nullptr, &outlen, in, plaintext.size()) <= 0)
        throw std::runtime_error("EVP_PKEY_encrypt (size query) failed");

    std::vector<unsigned char> out(outlen);
    if (EVP_PKEY_encrypt(ectx, out.data(), &outlen, in, plaintext.size()) <= 0) {
        char errbuf[256];
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
        throw std::runtime_error(std::string("gt_rsa_encrypt failed: ") + errbuf);
    }
    out.resize(outlen);

    std::ostringstream os;
    for (auto b : out)
        os << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return os.str();
}

static std::string hash_hex(const std::string& func, const std::string& data) {
    const EVP_MD* md = nullptr;
    if      (func == "md5")    md = EVP_md5();
    else if (func == "sha1")   md = EVP_sha1();
    else if (func == "sha256") md = EVP_sha256();
    else throw std::runtime_error("Unsupported hashfunc: " + func);

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  dlen = 0;
    EVP_Digest(data.data(), data.size(), digest, &dlen, md, nullptr);

    std::ostringstream os;
    for (unsigned int i = 0; i < dlen; ++i)
        os << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return os.str();
}


struct PowResult { std::string pow_msg; std::string pow_sign; };

static PowResult proof_of_work(
    const std::string& lot_number,
    const std::string& captcha_id,
    const std::string& version,
    const std::string& hashfunc,
    int                bits,
    const std::string& datetime_str)
{
    if (bits == 0) {
        std::string h        = util::new_uuid_v4();
        h.erase(std::remove(h.begin(), h.end(), '-'), h.end());
        std::string pow_msg  = version + "|" + std::to_string(bits) + "|" +
                               hashfunc + "|" + datetime_str + "|" +
                               captcha_id + "|" + lot_number + "||" + h;
        std::string pow_sign = hash_hex(hashfunc, pow_msg);
        return {pow_msg, pow_sign};
    }

    int a = bits % 4;
    int b = bits / 4;
    std::string u(b, '0');
    std::string prefix = version + "|" + std::to_string(bits) + "|" +
                         hashfunc + "|" + datetime_str + "|" +
                         captcha_id + "|" + lot_number + "||";
    static const int threshold[] = {0, 7, 3, 1};

    while (true) {
        std::string h = util::new_uuid_v4();
        h.erase(std::remove(h.begin(), h.end(), '-'), h.end());
        std::string pow_msg  = prefix + h;
        std::string pow_sign = hash_hex(hashfunc, pow_msg);

        if (a == 0) {
            if (pow_sign.substr(0, b) == u)
                return {pow_msg, pow_sign};
        } else {
            if (pow_sign.size() > (size_t)b &&
                pow_sign.substr(0, b) == u) {
                int d = std::stoi(pow_sign.substr(b, 1), nullptr, 16);
                if (d <= threshold[a])
                    return {pow_msg, pow_sign};
            }
        }
    }
}

static double calculate_user_response(int set_left, int img_width) {
    return set_left / (0.8876 * 340.0 / img_width) + 2.0;
}

static std::string generate_w(
    int                set_left,
    int                pass_time,
    const std::string& lot_number,
    const json&        pow_detail,
    const std::string& captcha_id,
    int                img_width)
{
    std::string version   = pow_detail.value("version",  "1");
    std::string hashfunc  = pow_detail.value("hashfunc", "md5");
    int         bits      = std::stoi(json_scalar_to_string(pow_detail, "bits", "0"));
    std::string dt        = pow_detail.value("datetime", "");

    auto pow_res = proof_of_work(lot_number, captcha_id, version, hashfunc, bits, dt);

    json em = {
        {"ph", 0}, {"cp", 0}, {"ek", "11"},
        {"wd", 1}, {"nt", 0}, {"si", 0}, {"sc", 0}
    };

    json params = {
        {"setLeft",      set_left},
        {"passtime",     pass_time},
        {"userresponse", calculate_user_response(set_left, img_width)},
        {"device_id",    ""},
        {"lot_number",   lot_number},
        {"pow_msg",      pow_res.pow_msg},
        {"pow_sign",     pow_res.pow_sign},
        {"geetest",      "captcha"},
        {"lang",         "zh"},
        {"ep",           "123"},
        {"biht",         "1426265548"},
        {"em",           em},
    };

    if (lot_number.size() >= 28) {
        std::string key = lot_number.substr(1, 4);
        std::string val = lot_number.substr(24, 4);
        params[key]     = val;
    }

    std::string params_json = params.dump();

    std::string aes_key = util::new_uuid_v4();
    aes_key.erase(std::remove(aes_key.begin(), aes_key.end(), '-'), aes_key.end());

    std::string rsa_enc_key = gt_rsa_encrypt_hex(aes_key);
    std::string aes_enc     = aes_cbc_encrypt_hex(params_json, aes_key);

    return aes_enc + rsa_enc_key;
}

static std::optional<json> geetest_verify(
    HttpSession&       session,
    const std::string& callback,
    const std::string& captcha_id,
    const std::string& lot_number,
    const std::string& risk_type,
    const std::string& payload,
    const std::string& process_token,
    const std::string& payload_protocol,
    const std::string& pt,
    const std::string& w)
{
    std::string path =
        "/verify?callback="         + util::url_encode(callback) +
        "&captcha_id="              + util::url_encode(captcha_id) +
        "&client_type=web"          +
        "&lot_number="              + util::url_encode(lot_number) +
        "&risk_type="               + util::url_encode(risk_type) +
        "&payload="                 + util::url_encode(payload) +
        "&process_token="           + util::url_encode(process_token) +
        "&payload_protocol="        + util::url_encode(payload_protocol) +
        "&pt="                      + util::url_encode(pt) +
        "&w="                       + util::url_encode(w);

    httplib::Headers hdrs = {
        {"Accept",                   "*/*"},
        {"Accept-Language",          "zh-CN,zh;q=0.9"},
        {"Connection",               "keep-alive"},
        {"Referer",                  "https://user.mihoyo.com/"},
        {"Sec-Fetch-Dest",           "script"},
        {"Sec-Fetch-Mode",           "no-cors"},
        {"Sec-Fetch-Site",           "cross-site"},
        {"User-Agent",               "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36 Edg/150.0.0.0"},
    };

    httplib::SSLClient cli("gcaptcha4.geetest.com", 443);
    cli.set_connection_timeout(15, 0);
    cli.set_read_timeout(20, 0);
    auto res = cli.Get(path, hdrs);
    if (!res) {
        LOG_ERROR("[geetest/verify] request failed: {}", httplib::to_string(res.error()));
        return std::nullopt;
    }
    LOG_INFO("[geetest/verify] status={}", res->status);

    std::string text = res->body;
    // trim
    while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
    while (!text.empty() && std::isspace((unsigned char)text.front())) text = text.substr(1);

    std::string json_str;
    if (text.rfind(callback, 0) == 0) {
        json_str = text.substr(callback.size());
        while (!json_str.empty() && (json_str.front() == '(' || std::isspace((unsigned char)json_str.front())))
            json_str = json_str.substr(1);
        while (!json_str.empty() && (json_str.back() == ')' || std::isspace((unsigned char)json_str.back())))
            json_str.pop_back();
    } else {
        json_str = text;
    }
    try {
        return json::parse(json_str);
    } catch (...) {
        LOG_ERROR("[geetest/verify] JSON parse failed");
        return std::nullopt;
    }
}

static std::string solve_aigis_captcha(
    HttpSession&                        session,
    const std::string&                  aigis_raw)
{
    if (aigis_raw.empty()) {
        LOG_ERROR("[aigis] x-rpc-aigis header missing");
        return "";
    }

    json aigis;
    try { aigis = json::parse(aigis_raw); }
    catch (...) { LOG_ERROR("[aigis] parse x-rpc-aigis failed"); return ""; }

    std::string session_id     = aigis.value("session_id", "");
    std::string inner_data_str = aigis.value("data", "{}");
    json inner_data;
    try { inner_data = json::parse(inner_data_str); }
    catch (...) { LOG_ERROR("[aigis] parse aigis.data failed"); return ""; }

    std::string captcha_id = inner_data.value("gt",        "");
    std::string risk_type  = inner_data.value("risk_type", "slide");
    if (captcha_id.empty()) { LOG_ERROR("[aigis] captcha_id empty"); return ""; }

    std::string timestamp = std::to_string(util::now_ms());
    std::string callback  = "geetest_" + timestamp;
    std::string challenge = util::new_uuid_v4();
    std::string user_info = "{\"session_id\":\"" + session_id + "\"}";

    std::string load_path =
        "/load?callback="    + util::url_encode(callback) +
        "&captcha_id="       + util::url_encode(captcha_id) +
        "&challenge="        + util::url_encode(challenge) +
        "&client_type=web"   +
        "&risk_type="        + util::url_encode(risk_type) +
        "&user_info="        + util::url_encode(user_info) +
        "&lang=zho";

    httplib::Headers load_hdrs = {
        {"Accept",             "*/*"},
        {"Accept-Language",    "zh-CN,zh;q=0.9"},
        {"Connection",         "keep-alive"},
        {"Referer",            "https://user.mihoyo.com/"},
        {"Sec-Fetch-Dest",     "script"},
        {"Sec-Fetch-Mode",     "no-cors"},
        {"Sec-Fetch-Site",     "cross-site"},
        {"User-Agent",         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36 Edg/150.0.0.0"},
    };

    httplib::SSLClient gt_cli("gcaptcha4.geetest.com", 443);
    gt_cli.set_connection_timeout(15, 0);
    gt_cli.set_read_timeout(15, 0);
    auto load_res = gt_cli.Get(load_path, load_hdrs);
    if (!load_res) {
        LOG_ERROR("[geetest/load] request failed: {}", httplib::to_string(load_res.error()));
        return "";
    }

    std::string load_text = load_res->body;
    while (!load_text.empty() && std::isspace((unsigned char)load_text.back()))  load_text.pop_back();
    while (!load_text.empty() && std::isspace((unsigned char)load_text.front())) load_text = load_text.substr(1);

    json load_data;
    if (load_text.rfind(callback, 0) == 0) {
        std::string js = load_text.substr(callback.size());
        while (!js.empty() && (js.front() == '(' || std::isspace((unsigned char)js.front()))) js = js.substr(1);
        while (!js.empty() && (js.back()  == ')' || std::isspace((unsigned char)js.back())))  js.pop_back();
        try { load_data = json::parse(js); }
        catch (...) { LOG_ERROR("[geetest/load] JSONP parse failed"); return ""; }
    } else {
        LOG_ERROR("[geetest/load] unexpected format");
        return "";
    }

    if (load_data.value("status", "") != "success") {
        LOG_ERROR("[geetest/load] status not success");
        return "";
    }

    auto& gt_data        = load_data["data"];
    std::string lot_number        = gt_data["lot_number"].get<std::string>();
    std::string slice_path        = gt_data["slice"].get<std::string>();
    std::string bg_path           = gt_data["bg"].get<std::string>();
    json        pow_detail        = gt_data["pow_detail"];
    std::string payload           = gt_data["payload"].get<std::string>();
    std::string process_token     = gt_data["process_token"].get<std::string>();
    std::string payload_protocol  = json_scalar_to_string(gt_data, "payload_protocol", "1");
    std::string pt                = json_scalar_to_string(gt_data, "pt", "1");
    std::string captcha_type      = gt_data.value("captcha_type", risk_type);

    httplib::Headers img_hdrs = {
        {"Accept",          "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8"},
        {"Accept-Language", "zh-CN,zh;q=0.9"},
        {"Connection",      "keep-alive"},
        {"Referer",         "https://user.mihoyo.com/"},
        {"Sec-Fetch-Dest",  "image"},
        {"Sec-Fetch-Mode",  "no-cors"},
        {"Sec-Fetch-Site",  "cross-site"},
        {"User-Agent",      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36 Edg/150.0.0.0"},
    };

    auto download_image = [&](const std::string& path) -> std::string {
        std::string url_path = "/" + path;
        if (!path.empty() && path[0] == '/') url_path = path;
        httplib::SSLClient img_cli("static.geetest.com", 443);
        img_cli.set_connection_timeout(15, 0);
        img_cli.set_read_timeout(15, 0);
        auto r = img_cli.Get(url_path, img_hdrs);
        if (!r || r->status != 200)
            throw std::runtime_error("download_image failed: " + path);
        return r->body;
    };

    std::string slice_bytes, bg_bytes;
    try {
        slice_bytes = download_image(slice_path);
        bg_bytes    = download_image(bg_path);
    } catch (const std::exception& e) {
        LOG_ERROR("[aigis] image download failed: {}", e.what());
        return "";
    }

    offset_estimator::OffsetResult offset_res;
    try {
        offset_res = offset_estimator::estimate_offset_from_bytes(slice_bytes, bg_bytes);
    } catch (const std::exception& e) {
        LOG_ERROR("[aigis] offset estimation failed: {}", e.what());
        return "";
    }
    int set_left  = (int)std::round(offset_res.horizontal_offset);
    int img_width = offset_res.image_width;

    std::uniform_int_distribution<int> pass_dist(1800, 2400);
    std::uniform_int_distribution<int> jitter_dist(0, 200);
    int pass_time = pass_dist(util::rng()) + jitter_dist(util::rng());
    LOG_INFO("[aigis] random time={} ms, sleeping...", pass_time);
    std::this_thread::sleep_for(std::chrono::milliseconds(pass_time));

    std::string w;
    try {
        w = generate_w(set_left, pass_time, lot_number, pow_detail, captcha_id, img_width);
    } catch (const std::exception& e) {
        LOG_ERROR("[aigis] generate_w failed: {}", e.what());
        return "";
    }

    auto verify_result = geetest_verify(
        session, callback, captcha_id, lot_number,
        captcha_type, payload, process_token,
        payload_protocol, pt, w);

    if (!verify_result) { LOG_ERROR("[aigis] /verify failed"); return ""; }

    std::string verify_status = verify_result->value("status", "");
    auto& vdata               = (*verify_result)["data"];
    std::string result_field  = vdata.value("result", "");
    auto& seccode             = vdata["seccode"];

    LOG_INFO("[geetest/verify] result={}", result_field);
    if (verify_status != "success" || result_field != "success") {
        LOG_ERROR("[aigis] geetest verify not passed");
        return "";
    }

    json aigis_data = {
        {"lot_number",     lot_number},
        {"captcha_id",     captcha_id},
        {"pass_token",     seccode.value("pass_token",     "")},
        {"gen_time",       seccode.value("gen_time",       "")},
        {"captcha_output", seccode.value("captcha_output", "")},
        {"userInfo",       user_info},
    };
    std::string aigis_json = aigis_data.dump();

    BIO* b64  = BIO_new(BIO_f_base64());
    BIO* bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, aigis_json.data(), (int)aigis_json.size());
    BIO_flush(b64);
    const char* bdata = nullptr;
    long blen = BIO_get_mem_data(bmem, &bdata);
    std::string aigis_data_b64(bdata, blen);
    BIO_free_all(b64);

    return session_id + ";" + aigis_data_b64;
}

} // namespace geetest

namespace custom_b64 {

static const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char PAD       = '=';

static std::string encode_hex(const std::string& hex) {
    std::string r;
    r.reserve(hex.size() / 3 * 2 + 4);
    size_t i = 0;
    while (i + 3 <= hex.size()) {
        int n = std::stoi(hex.substr(i, 3), nullptr, 16);
        r += CHARSET[n >> 6];
        r += CHARSET[63 & n];
        i += 3;
    }
    if (i + 1 == hex.size()) {
        int n = std::stoi(hex.substr(i, 1), nullptr, 16);
        r += CHARSET[n << 2];
    } else if (i + 2 == hex.size()) {
        int n = std::stoi(hex.substr(i, 2), nullptr, 16);
        r += CHARSET[n >> 2];
        r += CHARSET[(3 & n) << 4];
    }
    while (r.size() % 4) r += PAD;
    return r;
}

} // namespace custom_b64

class CryptoService {
public:
    CryptoService() : _pkey(nullptr, EVP_PKEY_free) {
        static const char* PUB_B64 =
            "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDDvekdPMHN3AYhm/vktJT+YJr7"
            "cI5DcsNKqdsx5DZX0gDuWFuIjzdwButrIYPNmRJ1G8ybDIF7oDW2eEpm5sMbL9zs"
            "9ExXCdvqrn51qELbqj0XxtMTIpaCHFSI50PfPpTFV9Xt/hmyVwokoOXFlAEgCn+Q"
            "CgGs52bFoYMtyi+xEQIDAQAB";

        BIO* b64bio = BIO_new(BIO_f_base64());
        BIO_set_flags(b64bio, BIO_FLAGS_BASE64_NO_NL);
        BIO* mem = BIO_new_mem_buf(PUB_B64, -1);
        BIO* chain = BIO_push(b64bio, mem);
        std::vector<unsigned char> der(512);
        int len = BIO_read(chain, der.data(), (int)der.size());
        BIO_free_all(chain);
        if (len <= 0) throw std::runtime_error("Failed to decode public key base64");
        der.resize(len);

        const unsigned char* p = der.data();
        EVP_PKEY* pkey = d2i_PUBKEY(nullptr, &p, (long)der.size());
        if (!pkey) throw std::runtime_error("Failed to parse public key DER");
        _pkey.reset(pkey);
    }

    std::string encrypt(const std::string& plaintext) {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(_pkey.get(), nullptr);
        if (!ctx) throw std::runtime_error("EVP_PKEY_CTX_new failed");
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx_guard(ctx, EVP_PKEY_CTX_free);

        if (EVP_PKEY_encrypt_init(ctx) <= 0)
            throw std::runtime_error("EVP_PKEY_encrypt_init failed");
        if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0)
            throw std::runtime_error("set_rsa_padding failed");

        size_t outlen = 0;
        const unsigned char* in = (const unsigned char*)plaintext.data();
        if (EVP_PKEY_encrypt(ctx, nullptr, &outlen, in, plaintext.size()) <= 0)
            throw std::runtime_error("EVP_PKEY_encrypt (size) failed");

        std::vector<unsigned char> out(outlen);
        if (EVP_PKEY_encrypt(ctx, out.data(), &outlen, in, plaintext.size()) <= 0) {
            char errbuf[256];
            ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
            throw std::runtime_error(std::string("EVP_PKEY_encrypt failed: ") + errbuf);
        }
        out.resize(outlen);

        std::ostringstream os;
        for (unsigned char b : out)
            os << std::hex << std::setw(2) << std::setfill('0') << (int)b;
        std::string hex_str = os.str();

        return custom_b64::encode_hex(hex_str);
    }

private:
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> _pkey;
    int _rsa_size = 0;
};

struct DeviceProfile {
    std::string device_id;
    std::string seed_id;
    std::string seed_time;
    std::string device_fp;
    std::string lifecycle_id;
    std::string webapp_lifecycle_id;
};

struct StableExtFields {
    std::string device_memory;
    std::string hardware_concurrency;
    std::string canvas;
    std::string webgl;

    static StableExtFields generate() {
        std::uniform_int_distribution<int> dm(0,2);
        std::uniform_int_distribution<int> dc(0,3);
        static const int mem_opts[] = {8,16,32};
        static const int cpu_opts[] = {4,8,12,16};
        return {
            std::to_string(mem_opts[dm(util::rng())]),
            std::to_string(cpu_opts[dc(util::rng())]),
            util::random_hex(64),
            util::random_hex(64),
        };
    }
};

struct SessionContext {
    DeviceProfile device;
    std::string   open_id;
    std::string   combo_token_raw;
};

struct LoginResult {
    SessionContext ctx;
    StableExtFields stable;
};

struct CookieInfo {
    std::string value;
    std::string domain;
    std::string path;
};

struct CacheEntry {
    std::string device_id;
    std::string seed_id;
    std::string seed_time;
    std::string device_fp;
    std::string lifecycle_id;
    std::string webapp_lifecycle_id;
    std::string open_id;
    std::string combo_token_raw;
    std::map<std::string, CookieInfo> cookies;
    std::string ext_device_memory;
    std::string ext_hardware_concurrency;
    std::string ext_canvas;
    std::string ext_webgl;
    long long   saved_at = 0;

    bool has_stable_ext() const {
        return !ext_device_memory.empty() && !ext_hardware_concurrency.empty()
            && !ext_canvas.empty() && !ext_webgl.empty();
    }
    StableExtFields get_stable_ext() const {
        return {ext_device_memory, ext_hardware_concurrency, ext_canvas, ext_webgl};
    }

    json to_json() const {
        json cookie_obj = json::object();
        for (auto& [name, ci] : cookies) {
            cookie_obj[name] = {
                {"value",  ci.value},
                {"domain", ci.domain},
                {"path",   ci.path},
            };
        }
        return {
            {"device_id",                 device_id},
            {"seed_id",                   seed_id},
            {"seed_time",                 seed_time},
            {"device_fp",                 device_fp},
            {"lifecycle_id",              lifecycle_id},
            {"webapp_lifecycle_id",       webapp_lifecycle_id},
            {"open_id",                   open_id},
            {"combo_token_raw",           combo_token_raw},
            {"cookies",                   cookie_obj},
            {"ext_device_memory",         ext_device_memory},
            {"ext_hardware_concurrency",  ext_hardware_concurrency},
            {"ext_canvas",                ext_canvas},
            {"ext_webgl",                 ext_webgl},
            {"saved_at",                  saved_at},
        };
    }

    static std::optional<CacheEntry> from_json(const json& v) {
        if (!v.contains("device_id") || !v.contains("open_id")
            || !v.contains("combo_token_raw") || !v.contains("cookies"))
            return std::nullopt;

        CacheEntry e;
        e.device_id           = v["device_id"].get<std::string>();
        e.seed_id             = v["seed_id"].get<std::string>();
        e.seed_time           = v["seed_time"].get<std::string>();
        e.device_fp           = v["device_fp"].get<std::string>();
        e.lifecycle_id        = v["lifecycle_id"].get<std::string>();
        e.webapp_lifecycle_id = v["webapp_lifecycle_id"].get<std::string>();
        e.open_id             = v["open_id"].get<std::string>();
        e.combo_token_raw     = v["combo_token_raw"].get<std::string>();
        e.saved_at            = v.contains("saved_at") ? v["saved_at"].get<long long>() : 0;

        if (v.contains("ext_device_memory"))
            e.ext_device_memory        = v["ext_device_memory"].get<std::string>();
        if (v.contains("ext_hardware_concurrency"))
            e.ext_hardware_concurrency = v["ext_hardware_concurrency"].get<std::string>();
        if (v.contains("ext_canvas"))
            e.ext_canvas               = v["ext_canvas"].get<std::string>();
        if (v.contains("ext_webgl"))
            e.ext_webgl                = v["ext_webgl"].get<std::string>();

        const auto& cobj = v["cookies"];
        if (cobj.is_object()) {
            for (auto& [name, ci] : cobj.items()) {
                CookieInfo info;
                info.value  = ci.contains("value")  ? ci["value"].get<std::string>()  : "";
                info.domain = ci.contains("domain") ? ci["domain"].get<std::string>() : "";
                info.path   = ci.contains("path")   ? ci["path"].get<std::string>()   : "";
                e.cookies[name] = std::move(info);
            }
        }
        return e;
    }
};

class CacheRepository {
public:
    static std::string default_path() {
        fs::path cache_dir;

#if defined(_WIN32)
        const char* appdata = std::getenv("APPDATA");
        if (appdata && *appdata)
            cache_dir = fs::path(appdata) / "cloudgame_checkin";
        else
            cache_dir = fs::current_path() / "cloudgame_checkin";
#else
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        if (xdg && *xdg)
            cache_dir = fs::path(xdg) / "cloudgame_checkin";
        else {
            const char* home = std::getenv("HOME");
            if (home && *home)
                cache_dir = fs::path(home) / ".cache" / "cloudgame_checkin";
            else
                cache_dir = fs::current_path() / "cloudgame_checkin";
        }
#endif
        std::error_code ec;
        fs::create_directories(cache_dir, ec);
        if (ec)
            LOG_WARN("Failed to create cache directory {}: {}", cache_dir.string(), ec.message());

        return (cache_dir / "session_cache.json").string();
    }

    explicit CacheRepository(std::string path = "") {
        _path = path.empty() ? default_path() : std::move(path);
    }

    std::optional<CacheEntry> load() const {
        std::ifstream f(_path);
        if (!f.is_open()) return std::nullopt;
        try {
            json v = json::parse(f);
            auto e = CacheEntry::from_json(v);
            if (!e) { LOG_WARN("Cache file is malformed, ignoring."); }
            return e;
        } catch (...) {
            LOG_WARN("Failed to parse cache file, ignoring.");
            return std::nullopt;
        }
    }

    void save(const CacheEntry& entry) const {
        std::ofstream f(_path, std::ios::trunc);
        if (!f.is_open()) throw std::runtime_error("Cannot write cache: " + _path);
        f << entry.to_json().dump(2);
        LOG_DEBUG("Session cache saved to {}", _path);
    }

    void remove() const {
        if (std::remove(_path.c_str()) == 0)
            LOG_INFO("Expired session cache deleted.");
    }

private:
    std::string _path;
};

struct Cookie {
    std::string name;
    std::string value;
    std::string domain;
    std::string path;
};

class CookieJar {
public:
    void set(const std::string& name, const std::string& value,
             const std::string& domain = "", const std::string& path = "/") {
        for (auto& c : _cookies) {
            if (c.name == name && c.domain == domain) { c.value = value; return; }
        }
        _cookies.push_back({name, value, domain, path});
    }

    std::string get(const std::string& name, const std::string& domain = "") const {
        for (auto& c : _cookies)
            if (c.name == name && (domain.empty() || c.domain == domain)) return c.value;
        return "";
    }

    std::string header_for(const std::string& host) const {
        std::string r;
        for (auto& c : _cookies) {
            std::string d = c.domain;
            if (!d.empty() && d[0] == '.') d = d.substr(1);
            if (!d.empty() && host.find(d) == std::string::npos) continue;
            if (!r.empty()) r += "; ";
            r += c.name + "=" + c.value;
        }
        return r;
    }

    const std::vector<Cookie>& all() const { return _cookies; }

    void restore_from_map(const std::map<std::string, CookieInfo>& m) {
        for (auto& [name, ci] : m)
            set(name, ci.value, ci.domain, ci.path);
    }

    std::map<std::string, CookieInfo> to_map() const {
        std::map<std::string, CookieInfo> m;
        for (auto& c : _cookies)
            m[c.name] = {c.value, c.domain, c.path};
        return m;
    }

private:
    std::vector<Cookie> _cookies;
};

class HttpSession {
public:
    CookieJar cookies;

	struct Response {
		int         status = 0;
		std::string body;
		std::string aigis_header;
		json parse_json() const { return json::parse(body); }
	};

    Response get(const std::string& url, const httplib::Headers& extra_headers = {}) {
        return request("GET", url, extra_headers, "", "");
    }

    Response post(const std::string& url,
                  const httplib::Headers& extra_headers = {},
                  const std::string& body = "",
                  const std::string& content_type = "application/json") {
        return request("POST", url, extra_headers, body, content_type);
    }

private:
    std::map<std::string, std::shared_ptr<httplib::SSLClient>> _clients;

    std::shared_ptr<httplib::SSLClient> get_client(const std::string& host, int port) {
        std::string key = host + ":" + std::to_string(port);
        auto it = _clients.find(key);
        if (it != _clients.end()) return it->second;
        auto cli = std::make_shared<httplib::SSLClient>(host, port);
        cli->set_follow_location(true);
        cli->set_connection_timeout(15, 0);
        cli->set_read_timeout(30, 0);
        _clients[key] = cli;
        return cli;
    }

    static std::tuple<std::string,std::string,int,std::string>
    parse_url(const std::string& url) {
        std::string scheme, host, pathq;
        int port = 443;
        size_t p = url.find("://");
        if (p == std::string::npos) throw std::runtime_error("Bad URL: " + url);
        scheme = url.substr(0, p);
        std::string rest = url.substr(p + 3);
        size_t slash = rest.find('/');
        std::string host_port = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        pathq = (slash == std::string::npos) ? "/" : rest.substr(slash);
        size_t colon = host_port.find(':');
        if (colon != std::string::npos) {
            host = host_port.substr(0, colon);
            port = std::stoi(host_port.substr(colon + 1));
        } else {
            host = host_port;
            port = (scheme == "https") ? 443 : 80;
        }
        return {scheme, host, port, pathq};
    }

	Response request(const std::string& method,
					 const std::string& url,
					 const httplib::Headers& extra_headers,
					 const std::string& body,
					 const std::string& content_type) {
		auto [scheme, host, port, pathq] = parse_url(url);

		httplib::Headers hdrs = extra_headers;
		std::string cookie_str = cookies.header_for(host);
		if (!cookie_str.empty())
			hdrs.emplace("Cookie", cookie_str);

		auto cli = get_client(host, port);

		httplib::Result res;
		if (method == "GET") {
			res = cli->Get(pathq, hdrs);
		} else if (method == "POST") {
			if (body.empty())
				res = cli->Post(pathq, hdrs);
			else
				res = cli->Post(pathq, hdrs, body, content_type);
		} else {
			throw std::runtime_error("Unsupported method: " + method);
		}

		if (!res) {
			std::string err = httplib::to_string(res.error());
			throw std::runtime_error("HTTP " + method + " " + url + " failed: " + err);
		}

		Response response;
		response.status = res->status;
		response.body   = res->body;

		for (auto& [k, v] : res->headers) {
			std::string kl = k;
			std::transform(kl.begin(), kl.end(), kl.begin(), ::tolower);
			if (kl == "x-rpc-aigis") {
				response.aigis_header = v;
			}

			if (kl == "set-cookie") {
				std::string name, value, domain, path = "/";
				std::istringstream ss(v);
				std::string token;
				bool first = true;
				while (std::getline(ss, token, ';')) {
					size_t s = token.find_first_not_of(" \t");
					size_t e = token.find_last_not_of(" \t");
					if (s == std::string::npos) continue;
					token = token.substr(s, e - s + 1);
					size_t eq = token.find('=');
					if (first) {
						first = false;
						if (eq != std::string::npos) {
							name  = token.substr(0, eq);
							value = token.substr(eq + 1);
						} else {
							name = token;
						}
					} else {
						std::string attr_key = (eq != std::string::npos) ? token.substr(0, eq) : token;
						std::string attr_val = (eq != std::string::npos) ? token.substr(eq + 1) : "";
						std::string attr_lower = attr_key;
						std::transform(attr_lower.begin(), attr_lower.end(), attr_lower.begin(), ::tolower);
						if (attr_lower == "domain") domain = attr_val;
						if (attr_lower == "path")   path   = attr_val;
					}
				}
				if (!name.empty())
					cookies.set(name, value, domain, path);
			}
		}

		return response;
	}
};

class HeaderBuilder {
public:
    HeaderBuilder() {
        _h = {
            {"User-Agent",         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36 Edg/150.0.0.0"},
            {"Accept",             "application/json, text/plain, */*"},
            {"Accept-Language",    "zh-CN,zh;q=0.9,en;q=0.8,en-GB;q=0.7,en-US;q=0.6"},
            {"Accept-Encoding",    "gzip, deflate, br, zstd"},
            {"Connection",         "keep-alive"},
            {"Sec-Fetch-Dest",     "empty"},
            {"Sec-Fetch-Mode",     "cors"},
            {"Sec-Fetch-Site",     "same-site"},
            {"sec-ch-ua",          "\"Not;A=Brand\";v=\"8\", \"Chromium\";v=\"150\", \"Microsoft Edge\";v=\"150\""},
            {"sec-ch-ua-mobile",   "?0"},
            {"sec-ch-ua-platform", "\"Windows\""},
        };
    }

    HeaderBuilder& with_origin(const std::string& origin, const std::string& referer) {
        set("Origin",  origin);
        set("Referer", referer);
        return *this;
    }

    HeaderBuilder& with_rpc_device(const std::string& device_id, const std::string& device_fp) {
        set("x-rpc-device_id",    device_id);
        set("x-rpc-device_fp",    device_fp);
        set("x-rpc-device_model", "Microsoft%20Edge%20150.0.0.0");
        set("x-rpc-device_name",  "Microsoft%20Edge");
        set("x-rpc-device_os",    "Windows%2010%2064-bit");
        return *this;
    }

    HeaderBuilder& with_rpc_app(const std::string& app_id,
                                const std::string& app_version,
                                const std::string& client_type,
                                const std::string& sdk_version) {
        set("x-rpc-app_id",      app_id);
        set("x-rpc-app_version", app_version);
        set("x-rpc-client_type", client_type);
        set("x-rpc-sdk_version", sdk_version);
        return *this;
    }

    HeaderBuilder& with_rpc_game(const std::string& game_biz,
                                 const std::string& lifecycle_id,
                                 const std::string& mi_referrer) {
        set("x-rpc-game_biz",     game_biz);
        set("x-rpc-lifecycle_id", lifecycle_id);
        set("x-rpc-mi_referrer",  mi_referrer);
        return *this;
    }

	HeaderBuilder& with_cloudgame(const std::string& combo_token,
								  const std::string& device_id,
								  const std::string& app_version) {
		set("x-rpc-app_id",       "4");
		set("x-rpc-app_version",  app_version);
		set("x-rpc-cg_game_biz",  "hk4e_cn");
		set("x-rpc-channel",      "mihoyo");
		set("x-rpc-client_type",  "16");
		set("x-rpc-combo_token",  combo_token);
		set("x-rpc-cps",          "pc_mihoyo");
		set("x-rpc-device_id",    device_id);
		set("x-rpc-device_model", "Unknown");
		set("x-rpc-device_name",  "Unknown");
		set("x-rpc-language",     "zh-cn");
		set("x-rpc-op_biz",       "clgm_cn");
		set("x-rpc-sys_version",  "Windows 10");
		set("x-rpc-vendor_id",    "2");
		return *this;
	}

    HeaderBuilder& with_extra(const std::string& key, const std::string& value) {
        set(key, value);
        return *this;
    }

    httplib::Headers build() const { return _h; }

private:
    httplib::Headers _h;

    void set(const std::string& k, const std::string& v) {
        _h.erase(k);
        _h.emplace(k, v);
    }
};

class MiHoYoApiClient {
public:
    explicit MiHoYoApiClient(HttpSession& session) : _s(session) {}

    std::string get_device_fp(const DeviceProfile& device,
                               const StableExtFields& stable) {
        json ext = {
            {"userAgent",           "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36 Edg/150.0.0.0"},
            {"browserScreenSize",   "1981440"},
            {"maxTouchPoints",      "0"},
            {"isTouchSupported",    "0"},
            {"browserLanguage",     "zh-CN"},
            {"browserPlat",         "Win32"},
            {"browserTimeZone",     "Asia/Shanghai"},
            {"webGlRender",         "ANGLE (Intel, Intel(R) UHD Graphics Direct3D11 vs_5_0 ps_5_0, D3D11)"},
            {"webGlVendor",         "Google Inc. (Intel)"},
            {"numOfPlugins",        "5"},
            {"listOfPlugins",       json::array({"PDF Viewer", "Chrome PDF Viewer", "Chromium PDF Viewer", "Microsoft Edge PDF Viewer", "WebKit built-in PDF"})},
            {"screenRatio",         "1"},
            {"deviceMemory",        stable.device_memory},
            {"hardwareConcurrency", stable.hardware_concurrency},
            {"cpuClass",            "unknown"},
            {"ifNotTrack",          "unknown"},
            {"ifAdBlock",           "0"},
            {"hasLiedLanguage",     "0"},
            {"hasLiedResolution",   "0"},
            {"hasLiedOs",           "0"},
            {"hasLiedBrowser",      "0"},
            {"canvas",              stable.canvas},
            {"webDriver",           "0"},
            {"colorDepth",          "24"},
            {"pixelRatio",          "1"},
            {"packageName",         "unknown"},
            {"packageVersion",      "2.54.0"},
            {"webgl",               stable.webgl},
        };

        std::string current_fp = device.device_fp.empty()
            ? util::random_hex(13) : device.device_fp;

        json payload = {
            {"device_id",  device.device_id},
            {"seed_id",    device.seed_id},
            {"seed_time",  device.seed_time},
            {"platform",   "22"},
            {"device_fp",  current_fp},
            {"app_name",   "hk4e_cn"},
            {"ext_fields", ext.dump()},
        };

        auto hdrs = HeaderBuilder()
            .with_origin("https://ys.mihoyo.com", "https://ys.mihoyo.com/")
            .with_extra("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36 Edg/150.0.0.0")
            .build();

        auto resp = _s.post(_FP_URL, hdrs, payload.dump());
        if (resp.status != 200)
            throw std::runtime_error("getFp HTTP " + std::to_string(resp.status));
        auto j = resp.parse_json();
        return j["data"]["device_fp"].get<std::string>();
    }

    HttpSession::Response web_verify_for_game(const DeviceProfile& device,
                                               const std::string& webapp_lifecycle_id) {
        std::string trace_info =
            "{\"webapp_lifecycle_id\":\"" + webapp_lifecycle_id + "\"}";
        _s.cookies.set(
            "MIHOYO_LOGIN_PLATFORM_COMMON_TRACE_INFO",
            util::url_encode(trace_info, ":%"),
            "mihoyo.com");

        auto hdrs = HeaderBuilder()
            .with_origin("https://ys.mihoyo.com", "https://ys.mihoyo.com/")
            .with_rpc_device(device.device_id, device.device_fp)
            .with_rpc_app("c76ync6mutq8", "", "22", "2.50.1")
            .with_rpc_game("hk4e_cn", device.lifecycle_id, "https://ys.mihoyo.com/cloud/#/")
            .build();

        return _s.post(_VERIFY_URL, hdrs, "", "application/json");
    }

	HttpSession::Response login_by_password(const DeviceProfile& device,
											 const std::string& enc_account,
											 const std::string& enc_password,
											 const std::string& aigis_token = "") {
        static const std::string mi_referrer =
            "https://user.mihoyo.com/login-platform/index.html"
            "?client_type=22&app_id=c76ync6mutq8&theme=ys&token_type=4"
            "&game_biz=hk4e_cn&message_origin=https%253A%252F%252Fys.mihoyo.com"
            "&succ_back_type=message%253Alogin-platform%253Alogin-success"
            "&fail_back_type=message%253Alogin-platform%253Alogin-fail"
            "&ux_mode=popup&iframe_level=1&extra_trace=1#/login/password";

		auto builder = HeaderBuilder()
			.with_origin("https://user.mihoyo.com", "https://user.mihoyo.com/")
			.with_rpc_device(device.device_id, device.device_fp)
			.with_rpc_app("c76ync6mutq8", "", "22", "2.54.0")
			.with_rpc_game("hk4e_cn", device.lifecycle_id, mi_referrer)
			.with_extra("x-rpc-source", "v2.webLogin");

		if (!aigis_token.empty())
			builder.with_extra("x-rpc-aigis", aigis_token);

		auto hdrs = builder.build();

		json body = {{"account", enc_account}, {"password", enc_password}};
		return _s.post(_LOGIN_PWD_URL, hdrs, body.dump());
	}

    HttpSession::Response web_login(const DeviceProfile& device) {
        auto hdrs = HeaderBuilder()
            .with_origin("https://ys.mihoyo.com", "https://ys.mihoyo.com/")
            .with_rpc_device(device.device_id, device.device_fp)
            .with_extra("Content-Type",      "application/json")
            .with_extra("x-rpc-client_type", "22")
            .with_extra("x-rpc-game_biz",    "hk4e_cn")
            .with_extra("x-rpc-channel_id",  "1")
            .with_extra("x-rpc-language",    "zh-cn")
            .with_extra("x-rpc-mdk_version", "2.52.0")
            .build();

        json body = {{"app_id", 4}, {"channel_id", 1}};
        return _s.post(_WEB_LOGIN_URL, hdrs, body.dump());
    }

	HttpSession::Response cloudgame_login(const SessionContext& ctx,
										  const std::string& app_version) {
		auto token = build_combo_token(ctx);
		auto hdrs = HeaderBuilder()
			.with_origin("https://ys.mihoyo.com", "https://ys.mihoyo.com/")
			.with_cloudgame(token, ctx.device.device_id, app_version)
			.build();
		return _s.post(_CG_LOGIN_URL, hdrs, "", "application/json");
	}

	HttpSession::Response get_wallet(const SessionContext& ctx,
									 const std::string& app_version) {
		auto hdrs = HeaderBuilder()
			.with_origin("https://ys.mihoyo.com", "https://ys.mihoyo.com/")
			.with_cloudgame(build_combo_token(ctx), ctx.device.device_id, app_version)
			.build();
		return _s.get(_WALLET_URL, hdrs);
	}

	HttpSession::Response list_notifications(const SessionContext& ctx,
											 const std::string& app_version) {
		auto hdrs = HeaderBuilder()
			.with_origin("https://ys.mihoyo.com", "https://ys.mihoyo.com/")
			.with_cloudgame(build_combo_token(ctx), ctx.device.device_id, app_version)
			.build();
		return _s.get(_NOTIF_URL, hdrs);
	}

	HttpSession::Response ack_notification(const SessionContext& ctx,
										   const std::string& reward_id,
										   const std::string& app_version) {
		auto hdrs = HeaderBuilder()
			.with_origin("https://ys.mihoyo.com", "https://ys.mihoyo.com/")
			.with_cloudgame(build_combo_token(ctx), ctx.device.device_id, app_version)
			.build();
		json body = {{"id", reward_id}};
		return _s.post(_ACK_URL, hdrs, body.dump());
	}

    static std::string build_combo_token(const SessionContext& ctx) {
        static const std::string app_id     = "4";
        static const std::string channel_id = "1";
        static const std::string app_key    = "d0d3a7342df2026a70f650b907800111";

        std::string query =
            "app_id="      + app_id     + "&" +
            "channel_id="  + channel_id + "&" +
            "combo_token=" + ctx.combo_token_raw + "&" +
            "open_id="     + ctx.open_id;

        std::string sig = util::hmac_sha256_hex(app_key, query);

        return "ai=" + app_id +
               ";ci=" + channel_id +
               ";oi=" + ctx.open_id +
               ";ct=" + ctx.combo_token_raw +
               ";si=" + sig +
               ";bi=hk4e_cn";
    }

private:
    HttpSession& _s;

    static constexpr const char* _FP_URL       = "https://public-data-api.mihoyo.com/device-fp/api/getFp";
    static constexpr const char* _VERIFY_URL    = "https://passport-api.mihoyo.com/account/ma-cn-session/web/webVerifyForGame";
    static constexpr const char* _LOGIN_PWD_URL = "https://passport-api.mihoyo.com/account/ma-cn-passport/web/loginByPassword";
    static constexpr const char* _WEB_LOGIN_URL = "https://hk4e-sdk.mihoyo.com/hk4e_cn/combo/granter/login/webLogin";
    static constexpr const char* _CG_LOGIN_URL  = "https://api-cloudgame.mihoyo.com/hk4e_cg_cn/gamer/api/login";
    static constexpr const char* _WALLET_URL    = "https://api-cloudgame.mihoyo.com/hk4e_cg_cn/wallet/wallet/get";
    static constexpr const char* _NOTIF_URL     = "https://api-cloudgame.mihoyo.com/hk4e_cg_cn/gamer/api/listNotifications"
                                                   "?status=NotificationStatusUnread&type=NotificationTypePopup&is_sort=true";
    static constexpr const char* _ACK_URL       = "https://api-cloudgame.mihoyo.com/hk4e_cg_cn/gamer/api/ackNotification";
};

class CloudGameService {
public:
    CloudGameService(MiHoYoApiClient& client, std::string app_version)
        : _client(client), _app_version(std::move(app_version)) {}

    void run_checkin(const SessionContext& ctx) {
        int free_minutes = print_wallet(ctx);
		claim_checkin_rewards(ctx, free_minutes);
    }

private:
    MiHoYoApiClient& _client;
    std::string      _app_version;

	int print_wallet(const SessionContext& ctx) {
		auto resp = _client.get_wallet(ctx, _app_version);
		if (resp.status != 200)
			throw std::runtime_error("get_wallet HTTP " + std::to_string(resp.status));
		auto j             = resp.parse_json();
		auto& data         = j["data"];
		std::string free_time     = data["free_time"]["free_time"].get<std::string>();
		std::string play_card_msg = data["play_card"]["short_msg"].get<std::string>();
		std::string coin_num      = data["coin"]["coin_num"].get<std::string>();

		double coin_minutes = 0;
		if (!coin_num.empty()) {
			try { coin_minutes = std::stod(coin_num) / 10.0; } catch (...) {}
		}
		std::ostringstream oss;
		oss << std::fixed << std::setprecision(0) << coin_minutes;
		LOG_INFO("Wallet | Free time {} min | Play Card '{}' | Coin_num {} pts (approx {} min)",
				 free_time, play_card_msg, coin_num, oss.str());

		int free_minutes = -1;
		if (!free_time.empty()) {
			try { free_minutes = std::stoi(free_time); } catch (...) {}
		}
		return free_minutes;
	}

	void claim_checkin_rewards(const SessionContext& ctx, int current_free_minutes) {
		auto resp = _client.list_notifications(ctx, _app_version);
		if (resp.status != 200)
			throw std::runtime_error("list_notifications HTTP " + std::to_string(resp.status));
		auto j    = resp.parse_json();
		auto& lst = j["data"]["list"];

		if (lst.is_null() || lst.size() == 0) {
			LOG_INFO("Check-in | Already checked in today, no new rewards.");
			return;
		}

		for (auto& notif : lst) {
			std::string reward_id = notif["id"].get<std::string>();

			auto ack = _client.ack_notification(ctx, reward_id, _app_version);

			if (ack.status != 200) {
				throw std::runtime_error(
					"ack_notification HTTP " + std::to_string(ack.status)
				);
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		
		auto wallet_resp = _client.get_wallet(ctx, _app_version);

		if (wallet_resp.status != 200) {
			throw std::runtime_error(
				"get_wallet after check-in HTTP "
				+ std::to_string(wallet_resp.status)
			);
		}

		auto wallet_json = wallet_resp.parse_json();

		std::string updated_free_time_str =
			wallet_json["data"]["free_time"]["free_time"].get<std::string>();

		int updated_free_minutes = -1;

		if (!updated_free_time_str.empty()) {
			try {
				updated_free_minutes = std::stoi(updated_free_time_str);
			} catch (...) {
				updated_free_minutes = -1;
			}
		}

		if (current_free_minutes >= 0 && updated_free_minutes >= 0) {
			int total_earned = updated_free_minutes - current_free_minutes;

			if (total_earned < 0) {
				LOG_WARN(
					"Check-in | Wallet balance decreased unexpectedly: {} -> {}",
					current_free_minutes,
					updated_free_minutes
				);
				total_earned = 0;
			}

			LOG_INFO(
				"Check-in | Total earned: {} min | Updated free time: {} min",
				total_earned,
				updated_free_minutes
			);
		} else {
			LOG_WARN("Check-in | Unable to calculate earned free time.");
		}
    }
};

class FullLoginStrategy {
public:
    FullLoginStrategy(
        MiHoYoApiClient&        client,
        CryptoService&          crypto,
        HttpSession&            session,
        const std::string&      account,
        const std::string&      password,
        const std::string&      pinned_device_id  = "",
        const std::string&      pinned_seed_id    = "",
        const std::string&      pinned_seed_time  = "",
        const std::string&      pinned_device_fp  = "",
        std::optional<StableExtFields> pinned_ext = std::nullopt,
		std::string             app_version       = "6.7.0"
    ) : _client(client), _crypto(crypto), _session(session),
        _account(account), _password(password),
        _pinned_device_id(pinned_device_id),
        _pinned_seed_id(pinned_seed_id),
        _pinned_seed_time(pinned_seed_time),
        _pinned_device_fp(pinned_device_fp),
        _pinned_ext(pinned_ext),
		_app_version(std::move(app_version))
    {}

    std::optional<LoginResult> execute() {
        auto [device, stable] = init_device();
        auto open_id = authenticate(device);
        if (!open_id) return std::nullopt;
        auto combo_token_raw = obtain_combo_token(device);
        if (!combo_token_raw) return std::nullopt;
        SessionContext ctx{device, *open_id, *combo_token_raw};
        if (!cloudgame_login(ctx)) return std::nullopt;
        return LoginResult{ctx, stable};
    }

private:
    MiHoYoApiClient&               _client;
    CryptoService&                 _crypto;
    HttpSession&                   _session;
    std::string                    _account;
    std::string                    _password;
    std::string                    _pinned_device_id;
    std::string                    _pinned_seed_id;
    std::string                    _pinned_seed_time;
    std::string                    _pinned_device_fp;
    std::optional<StableExtFields> _pinned_ext;
	std::string                    _app_version;

    std::pair<DeviceProfile, StableExtFields> init_device() {
        std::string device_id = _pinned_device_id.empty() ? util::new_uuid_v4()            : _pinned_device_id;
        std::string seed_id   = _pinned_seed_id.empty()   ? util::random_hex(16)           : _pinned_seed_id;
        std::string seed_time = _pinned_seed_time.empty() ? std::to_string(util::now_ms()) : _pinned_seed_time;
        StableExtFields stable = _pinned_ext ? *_pinned_ext : StableExtFields::generate();

        std::string lifecycle_id        = util::random_hex(10);
        std::string webapp_lifecycle_id = util::new_uuid_v4();

        _session.cookies.set("_MHYUUID",                           device_id,    "mihoyo.com");
        _session.cookies.set("DEVICEFP_SEED_ID",                   seed_id,      "mihoyo.com");
        _session.cookies.set("DEVICEFP_SEED_TIME",                 seed_time,    "mihoyo.com");
        _session.cookies.set("MIHOYO_LOGIN_PLATFORM_LIFECYCLE_ID", lifecycle_id, "mihoyo.com");

        DeviceProfile partial{
            device_id, seed_id, seed_time,
            _pinned_device_fp,
            lifecycle_id, webapp_lifecycle_id
        };

        std::string device_fp = _client.get_device_fp(partial, stable);
        _session.cookies.set("DEVICEFP", device_fp, "mihoyo.com");

        DeviceProfile device{
            device_id, seed_id, seed_time,
            device_fp, lifecycle_id, webapp_lifecycle_id
        };
        return {device, stable};
    }

	std::optional<std::string> authenticate(const DeviceProfile& device) {
		auto vresp = _client.web_verify_for_game(device, device.webapp_lifecycle_id);
		if (vresp.status == 200) {
			auto j = vresp.parse_json();
			if (j["retcode"].get<int>() == 0) {
				LOG_INFO("Session already valid (fast path).");
				return j["data"]["user_info"]["aid"].get<std::string>();
			}
		}

		LOG_INFO("No active session - attempting password login.");
		std::string enc_account  = _crypto.encrypt(_account);
		std::string enc_password = _crypto.encrypt(_password);

		auto lresp = _client.login_by_password(device, enc_account, enc_password);
		auto lj    = lresp.parse_json();

		if (lresp.status == 200 && lj["retcode"].get<int>() == -3101) {
			LOG_INFO("[loginByPassword] captcha triggered (-3101), solving...");
			std::string aigis_token = geetest::solve_aigis_captcha(
				_session, lresp.aigis_header);
			if (aigis_token.empty()) {
				LOG_ERROR("[aigis] captcha solving failed.");
				return std::nullopt;
			}
			LOG_INFO("[loginByPassword] retrying with aigis token.");
			lresp = _client.login_by_password(device, enc_account, enc_password, aigis_token);
			lj    = lresp.parse_json();
		}

		if (!(lresp.status == 200 && lj["retcode"].get<int>() == 0)) {
			LOG_ERROR("Password login failed: {}", lj.value("message", "unknown error"));
			return std::nullopt;
		}

		auto vresp2 = _client.web_verify_for_game(device, device.webapp_lifecycle_id);
		auto j2 = vresp2.parse_json();
		if (j2["retcode"].get<int>() != 0) {
			LOG_ERROR("Post-login verification failed.");
			return std::nullopt;
		}
		return j2["data"]["user_info"]["aid"].get<std::string>();
	}

    std::optional<std::string> obtain_combo_token(const DeviceProfile& device) {
        auto resp = _client.web_login(device);
        auto j    = resp.parse_json();
        if (!(resp.status == 200 && j["retcode"].get<int>() == 0)) {
            LOG_ERROR("webLogin failed: {}", j["message"].get<std::string>());
            return std::nullopt;
        }
        return j["data"]["combo_token"].get<std::string>();
    }

    bool cloudgame_login(const SessionContext& ctx) {
        auto resp = _client.cloudgame_login(ctx, _app_version);
        auto j    = resp.parse_json();
        if (!(resp.status == 200 && j["retcode"].get<int>() == 0)) {
            LOG_ERROR("Cloud-game login failed: {}", j["message"].get<std::string>());
            return false;
        }
        return true;
    }
};

class CachedLoginStrategy {
public:
    CachedLoginStrategy(MiHoYoApiClient& client,
                        HttpSession&     session,
                        const CacheEntry& cache)
        : _client(client), _session(session), _cache(cache) {}

    std::optional<LoginResult> execute() {
        restore_cookies();
        DeviceProfile device{
            _cache.device_id, _cache.seed_id, _cache.seed_time,
            _cache.device_fp, _cache.lifecycle_id, _cache.webapp_lifecycle_id
        };
        auto resp = _client.web_verify_for_game(device, device.webapp_lifecycle_id);
        if (!(resp.status == 200 && resp.parse_json()["retcode"].get<int>() == 0)) {
            LOG_WARN("Cached session has expired.");
            return std::nullopt;
        }
        LOG_INFO("Cached session is valid.");
        SessionContext ctx{device, _cache.open_id, _cache.combo_token_raw};
        StableExtFields stable = _cache.has_stable_ext()
            ? _cache.get_stable_ext() : StableExtFields::generate();
        return LoginResult{ctx, stable};
    }

private:
    MiHoYoApiClient&  _client;
    HttpSession&      _session;
    const CacheEntry& _cache;

    void restore_cookies() {
        _session.cookies.restore_from_map(_cache.cookies);
    }
};

class CheckInFacade {
public:
    CheckInFacade(std::string account, std::string password)
        : _account(std::move(account))
        , _password(std::move(password))
    {}

	void run() {
		std::string app_version = util::fetch_cloudgame_version();
        auto cache = _repo.load();

        HttpSession session;
        std::optional<LoginResult> result;

        if (cache) {
            std::time_t t = (std::time_t)cache->saved_at;
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            LOG_INFO("Found cached session (saved at {}), trying fast path.", std::string(buf));

            result = try_cached(session, *cache);
            if (!result) {
                LOG_INFO("Cached session expired, re-authenticating with pinned device identity.");
                session = HttpSession{};
                result  = try_full(session, &*cache, app_version);
                if (result) _repo.remove();
            }
        } else {
            LOG_INFO("No cache found, running full login flow.");
            result = try_full(session, nullptr, app_version); 
        }

        if (!result) {
            LOG_ERROR("All login attempts failed. Aborting.");
            return;
        }

        MiHoYoApiClient client(session);
        CloudGameService svc(client, app_version);
        svc.run_checkin(result->ctx);
        persist(session, result->ctx, result->stable);
    }

private:
    std::string     _account;
    std::string     _password;
    CacheRepository _repo;
    CryptoService   _crypto;

    std::optional<LoginResult> try_cached(HttpSession& session, const CacheEntry& cache) {
        MiHoYoApiClient client(session);
        CachedLoginStrategy strat(client, session, cache);
        return strat.execute();
    }

	std::optional<LoginResult> try_full(HttpSession& session, const CacheEntry* pinned,
										const std::string& app_version) {
		MiHoYoApiClient client(session);
		FullLoginStrategy strat(
			client, _crypto, session,
			_account, _password,
			pinned ? pinned->device_id   : "",
			pinned ? pinned->seed_id     : "",
			pinned ? pinned->seed_time   : "",
			pinned ? pinned->device_fp   : "",
			pinned && pinned->has_stable_ext()
				? std::make_optional(pinned->get_stable_ext())
				: std::nullopt,
			app_version
		);
		return strat.execute();
	}

    void persist(HttpSession& session,
                 const SessionContext& ctx,
                 const StableExtFields& stable) {
        CacheEntry entry;
        entry.device_id           = ctx.device.device_id;
        entry.seed_id             = ctx.device.seed_id;
        entry.seed_time           = ctx.device.seed_time;
        entry.device_fp           = ctx.device.device_fp;
        entry.lifecycle_id        = ctx.device.lifecycle_id;
        entry.webapp_lifecycle_id = ctx.device.webapp_lifecycle_id;
        entry.open_id             = ctx.open_id;
        entry.combo_token_raw     = ctx.combo_token_raw;
        entry.cookies             = session.cookies.to_map();
        entry.ext_device_memory        = stable.device_memory;
        entry.ext_hardware_concurrency = stable.hardware_concurrency;
        entry.ext_canvas               = stable.canvas;
        entry.ext_webgl                = stable.webgl;
        entry.saved_at            = util::now_s();
        _repo.save(entry);
    }
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " account password\n";
        return 1;
    }
    try {
#ifdef _WIN32
		SetConsoleOutputCP(CP_UTF8);
		SetConsoleCP(CP_UTF8);
#endif
        CheckInFacade(argv[1], argv[2]).run();
    } catch (const std::exception& ex) {
        LOG_ERROR("Unhandled exception: {}", ex.what());
        return 1;
    }
    return 0;
}
