// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// OAuthTokenManagerApp is a simple auth service hack for fetching user OAuth
// tokens to talk programmatically to backend apis. These apis are hosted or
// integrated with Identity providers such as Google, Twitter, Spotify etc.

#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <utility>

#include <fuchsia/net/oldhttp/cpp/fidl.h>
#include <fuchsia/ui/views_v1/cpp/fidl.h>
#include <fuchsia/webview/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <modular_auth/cpp/fidl.h>
#include <trace-provider/provider.h>

#include "lib/app/cpp/connect.h"
#include "lib/app/cpp/startup_context.h"
#include "lib/async/cpp/operation.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fidl/cpp/optional.h"
#include "lib/fidl/cpp/string.h"
#include "lib/fsl/socket/strings.h"
#include "lib/fsl/vmo/strings.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/files/directory.h"
#include "lib/fxl/files/file.h"
#include "lib/fxl/files/path.h"
#include "lib/fxl/log_settings_command_line.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/strings/join_strings.h"
#include "lib/fxl/strings/string_number_conversions.h"
#include "lib/fxl/time/time_point.h"
#include "lib/svc/cpp/services.h"
#include "third_party/rapidjson/rapidjson/document.h"
#include "third_party/rapidjson/rapidjson/error/en.h"
#include "third_party/rapidjson/rapidjson/pointer.h"
#include "third_party/rapidjson/rapidjson/prettywriter.h"
#include "third_party/rapidjson/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/rapidjson/writer.h"
#include "topaz/examples/oauth_token_manager/credentials_generated.h"

namespace fuchsia {
namespace modular {
namespace auth {

namespace http = ::fuchsia::net::oldhttp;

using ShortLivedTokenCallback =
    std::function<void(std::string, modular_auth::AuthErr)>;

using FirebaseTokenCallback =
    std::function<void(modular_auth::FirebaseTokenPtr, modular_auth::AuthErr)>;

namespace {

// TODO(alhaad/ukode): Move the following to a configuration file.
// NOTE: We are currently using a single client-id in Fuchsia. This is temporary
// and will change in the future.
constexpr char kClientId[] =
    "934259141868-rejmm4ollj1bs7th1vg2ur6antpbug79.apps.googleusercontent.com";
constexpr char kGoogleOAuthAuthEndpoint[] =
    "https://accounts.google.com/o/oauth2/v2/auth";
constexpr char kGoogleOAuthTokenEndpoint[] =
    "https://www.googleapis.com/oauth2/v4/token";
constexpr char kGoogleRevokeTokenEndpoint[] =
    "https://accounts.google.com/o/oauth2/revoke";
constexpr char kGooglePeopleGetEndpoint[] =
    "https://www.googleapis.com/plus/v1/people/me";
constexpr char kFirebaseAuthEndpoint[] =
    "https://www.googleapis.com/identitytoolkit/v3/relyingparty/"
    "verifyAssertion";
constexpr char kRedirectUri[] = "com.google.fuchsia.auth:/oauth2redirect";
constexpr char kCredentialsFile[] = "/data/v2/creds.db";
constexpr char kWebViewUrl[] = "web_view";

constexpr auto kScopes = {
    "openid",
    "email",
    "https://www.googleapis.com/auth/admin.directory.user.readonly",
    "https://www.googleapis.com/auth/assistant",
    "https://www.googleapis.com/auth/gmail.modify",
    "https://www.googleapis.com/auth/userinfo.email",
    "https://www.googleapis.com/auth/userinfo.profile",
    "https://www.googleapis.com/auth/youtube.readonly",
    "https://www.googleapis.com/auth/contacts",
    "https://www.googleapis.com/auth/drive",
    "https://www.googleapis.com/auth/plus.login",
    "https://www.googleapis.com/auth/calendar.readonly"};

// Type of token requested.
enum TokenType {
  ACCESS_TOKEN = 0,
  ID_TOKEN = 1,
  FIREBASE_JWT_TOKEN = 2,
};

// Adjusts the token expiration window by a small amount to proactively refresh
// tokens before the expiry time limit has reached.
const uint64_t kPaddingForTokenExpiryInS = 600;

template <typename T>
inline std::string JsonValueToPrettyString(const T& v) {
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  v.Accept(writer);
  return buffer.GetString();
}

// TODO(alhaad/ukode): Don't use a hand-rolled version of this.
std::string UrlEncode(const std::string& value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;

  for (char c : value) {
    // Keep alphanumeric and other accepted characters intact
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '=' ||
        c == '&' || c == '+') {
      escaped << c;
      continue;
    }

    // Any other characters are percent-encoded
    escaped << std::uppercase;
    escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
    escaped << std::nouppercase;
  }

  return escaped.str();
}

// Returns |::auth::CredentialStore| after parsing credentials from
// |kCredentialsFile|.
const ::auth::CredentialStore* ParseCredsFile() {
  // Reserialize existing users.
  if (!files::IsFile(kCredentialsFile)) {
    return nullptr;
  }

  std::string serialized_creds;
  if (!files::ReadFileToString(kCredentialsFile, &serialized_creds)) {
    FXL_LOG(WARNING) << "Unable to read user configuration file at: "
                     << kCredentialsFile;
    return nullptr;
  }

  flatbuffers::Verifier verifier(
      reinterpret_cast<const unsigned char*>(serialized_creds.data()),
      serialized_creds.size());
  if (!::auth::VerifyCredentialStoreBuffer(verifier)) {
    FXL_LOG(WARNING) << "Unable to verify credentials buffer:"
                     << serialized_creds.data();
    return nullptr;
  }

  return ::auth::GetCredentialStore(serialized_creds.data());
}

// Serializes |::auth::CredentialStore| to the |kCredentialsFIle| on disk.
bool WriteCredsFile(const std::string& serialized_creds) {
  // verify file before saving
  flatbuffers::Verifier verifier(
      reinterpret_cast<const unsigned char*>(serialized_creds.data()),
      serialized_creds.size());
  if (!::auth::VerifyCredentialStoreBuffer(verifier)) {
    FXL_LOG(ERROR) << "Unable to verify credentials buffer:"
                   << serialized_creds.data();
    return false;
  }

  if (!files::CreateDirectory(files::GetDirectoryName(kCredentialsFile))) {
    FXL_LOG(ERROR) << "Unable to create directory for " << kCredentialsFile;
    return false;
  }

  if (!files::WriteFile(kCredentialsFile, serialized_creds.data(),
                        serialized_creds.size())) {
    FXL_LOG(ERROR) << "Unable to write file " << kCredentialsFile;
    return false;
  }

  return true;
}

// Fetch user's refresh token from local credential store. In case of errors
// or account not found, an empty token is returned.
std::string GetRefreshTokenFromCredsFile(const std::string& account_id) {
  if (account_id.empty()) {
    FXL_LOG(ERROR) << "Account id is empty.";
    return "";
  }

  const ::auth::CredentialStore* credentials_storage = ParseCredsFile();
  if (credentials_storage == nullptr) {
    FXL_LOG(ERROR) << "Failed to parse credentials.";
    return "";
  }

  for (const auto* credential : *credentials_storage->creds()) {
    if (credential->account_id()->str() == account_id) {
      for (const auto* token : *credential->tokens()) {
        switch (token->identity_provider()) {
          case ::auth::IdentityProvider_GOOGLE:
            return token->refresh_token()->str();
          default:
            FXL_LOG(WARNING) << "Unrecognized IdentityProvider"
                             << token->identity_provider();
        }
      }
    }
  }
  return "";
}

// Exactly one of success_callback and failure_callback is ever invoked.
void Post(const std::string& request_body, http::URLLoader* const url_loader,
          const std::string& url, const std::function<void()>& success_callback,
          const std::function<void(modular_auth::Status, std::string)>&
              failure_callback,
          const std::function<bool(rapidjson::Document)>& set_token_callback) {
  std::string encoded_request_body(request_body);
  if (url.find(kFirebaseAuthEndpoint) == std::string::npos) {
    encoded_request_body = UrlEncode(request_body);
  }

  fsl::SizedVmo data;
  auto result = fsl::VmoFromString(encoded_request_body, &data);
  FXL_VLOG(1) << "Post Data:" << encoded_request_body;
  FXL_DCHECK(result);

  http::URLRequest request;
  request.url = url;
  request.method = "POST";
  request.auto_follow_redirects = true;

  // Content-length header.
  http::HttpHeader content_length_header;
  content_length_header.name = "Content-length";
  uint64_t data_size = encoded_request_body.length();
  content_length_header.value = fxl::NumberToString(data_size);
  request.headers.push_back(std::move(content_length_header));

  // content-type header.
  http::HttpHeader content_type_header;
  content_type_header.name = "content-type";
  if (url.find("identitytoolkit") != std::string::npos) {
    // set accept header
    http::HttpHeader accept_header;
    accept_header.name = "accept";
    accept_header.value = "application/json";
    request.headers.push_back(std::move(accept_header));

    // set content_type header
    content_type_header.value = "application/json";
  } else {
    content_type_header.value = "application/x-www-form-urlencoded";
  }
  request.headers.push_back(std::move(content_type_header));

  request.body = http::URLBody::New();
  request.body->set_sized_buffer(std::move(data).ToTransport());

  url_loader->Start(std::move(request), [success_callback, failure_callback,
                                         set_token_callback](
                                            http::URLResponse response) {
    FXL_VLOG(1) << "URL Loader response:"
                << std::to_string(response.status_code);

    if (response.error) {
      failure_callback(
          modular_auth::Status::NETWORK_ERROR,
          "POST error: " + std::to_string(response.error->code) +
              " , with description: " + response.error->description->data());
      return;
    }

    std::string response_body;
    if (response.body) {
      FXL_DCHECK(response.body->is_stream());
      // TODO(alhaad/ukode): Use non-blocking variant.
      if (!fsl::BlockingCopyToString(std::move(response.body->stream()),
                                     &response_body)) {
        failure_callback(modular_auth::Status::NETWORK_ERROR,
                         "Failed to read response from socket with status:" +
                             std::to_string(response.status_code));
        return;
      }
    }

    if (response.status_code != 200) {
      failure_callback(
          modular_auth::Status::OAUTH_SERVER_ERROR,
          "Received status code:" + std::to_string(response.status_code) +
              ", and response body:" + response_body);
      return;
    }

    rapidjson::Document doc;
    rapidjson::ParseResult ok = doc.Parse(response_body);
    if (!ok) {
      std::string error_msg = GetParseError_En(ok.Code());
      failure_callback(modular_auth::Status::BAD_RESPONSE,
                       "JSON parse error: " + error_msg);
      return;
    };
    auto result = set_token_callback(std::move(doc));
    if (result) {
      success_callback();
    } else {
      failure_callback(modular_auth::Status::BAD_RESPONSE,
                       "Invalid response: " + JsonValueToPrettyString(doc));
    }
    return;
  });
}

// Exactly one of success_callback and failure_callback is ever invoked.
void Get(http::URLLoader* const url_loader, const std::string& url,
         const std::string& access_token,
         const std::function<void()>& success_callback,
         const std::function<void(modular_auth::Status status, std::string)>&
             failure_callback,
         const std::function<bool(rapidjson::Document)>& set_token_callback) {
  http::URLRequest request;
  request.url = url;
  request.method = "GET";
  request.auto_follow_redirects = true;

  // Set Authorization header.
  http::HttpHeader auth_header;
  auth_header.name = "Authorization";
  auth_header.value = "Bearer " + access_token;
  request.headers.push_back(std::move(auth_header));

  // set content-type header to json.
  http::HttpHeader content_type_header;
  content_type_header.name = "content-type";
  content_type_header.value = "application/json";

  // set accept header to json
  http::HttpHeader accept_header;
  accept_header.name = "accept";
  accept_header.value = "application/json";
  request.headers.push_back(std::move(accept_header));

  url_loader->Start(std::move(request), [success_callback, failure_callback,
                                         set_token_callback](
                                            http::URLResponse response) {
    if (response.error) {
      failure_callback(
          modular_auth::Status::NETWORK_ERROR,
          "GET error: " + std::to_string(response.error->code) +
              " ,with description: " + response.error->description->data());
      return;
    }

    std::string response_body;
    if (response.body) {
      FXL_DCHECK(response.body->is_stream());
      // TODO(alhaad/ukode): Use non-blocking variant.
      if (!fsl::BlockingCopyToString(std::move(response.body->stream()),
                                     &response_body)) {
        failure_callback(modular_auth::Status::NETWORK_ERROR,
                         "Failed to read response from socket with status:" +
                             std::to_string(response.status_code));
        return;
      }
    }

    if (response.status_code != 200) {
      failure_callback(
          modular_auth::Status::OAUTH_SERVER_ERROR,
          "Status code: " + std::to_string(response.status_code) +
              " while fetching tokens with error description:" + response_body);
      return;
    }

    rapidjson::Document doc;
    rapidjson::ParseResult ok = doc.Parse(response_body);
    if (!ok) {
      std::string error_msg = GetParseError_En(ok.Code());
      failure_callback(modular_auth::Status::BAD_RESPONSE,
                       "JSON parse error: " + error_msg);
      return;
    };
    auto result = set_token_callback(std::move(doc));
    if (result) {
      success_callback();
    } else {
      failure_callback(modular_auth::Status::BAD_RESPONSE,
                       "Invalid response: " + JsonValueToPrettyString(doc));
    }
  });
}

}  // namespace

// Implementation of the OAuth Token Manager app.
class OAuthTokenManagerApp : modular_auth::AccountProvider {
 public:
  OAuthTokenManagerApp(async::Loop* loop);

 private:
  // |AccountProvider|
  void Initialize(fidl::InterfaceHandle<modular_auth::AccountProviderContext>
                      provider) override;

  // |AccountProvider|
  void Terminate() override;

  // |AccountProvider|
  void AddAccount(modular_auth::IdentityProvider identity_provider,
                  AddAccountCallback callback) override;

  // |AccountProvider|
  void RemoveAccount(modular_auth::Account account, bool revoke_all,
                     RemoveAccountCallback callback) override;

  // |AccountProvider|
  void GetTokenProviderFactory(
      fidl::StringPtr account_id,
      fidl::InterfaceRequest<modular_auth::TokenProviderFactory> request)
      override;

  // Generate a random account id.
  std::string GenerateAccountId();

  // Refresh access and id tokens.
  void RefreshToken(const std::string& account_id, const TokenType& token_type,
                    ShortLivedTokenCallback callback);

  // Refresh firebase tokens.
  void RefreshFirebaseToken(const std::string& account_id,
                            const std::string& firebase_api_key,
                            const std::string& id_token,
                            FirebaseTokenCallback callback);
  async::Loop* const loop_;

  std::shared_ptr<fuchsia::sys::StartupContext> startup_context_;

  modular_auth::AccountProviderContextPtr account_provider_context_;

  fidl::Binding<modular_auth::AccountProvider> binding_;

  class TokenProviderFactoryImpl;
  // account_id -> TokenProviderFactoryImpl
  std::unordered_map<std::string, std::unique_ptr<TokenProviderFactoryImpl>>
      token_provider_factory_impls_;

  // In-memory cache for long lived user credentials. This cache is populated
  // from |kCredentialsFile| on initialization.
  const ::auth::CredentialStore* creds_ = nullptr;

  // In-memory cache for short lived firebase auth id tokens. These tokens get
  // reset on system reboots. Tokens are cached based on the expiration time
  // set by the Firebase servers. Cache is indexed by firebase api keys.
  struct FirebaseAuthToken {
    uint64_t creation_ts;
    uint64_t expires_in;
    std::string id_token;
    std::string local_id;
    std::string email;
  };

  // In-memory cache for short lived oauth tokens that resets on system reboots.
  // Tokens are cached based on the expiration time set by the Identity
  // provider. Cache is indexed by unique account_ids.
  struct ShortLivedToken {
    uint64_t creation_ts;
    uint64_t expires_in;
    std::string access_token;
    std::string id_token;
    std::map<std::string, FirebaseAuthToken> fb_tokens_;
  };
  std::map<std::string, ShortLivedToken> oauth_tokens_;

  // We are using operations here not to guard state across asynchronous calls
  // but rather to clean up state after an 'operation' is done.
  // TODO(ukode): All operations are running in a queue now which is
  // inefficient because we block on operations that could be done in parallel.
  // Instead we may want to create an operation for what
  // TokenProviderFactoryImpl::GetFirebaseAuthToken() is doing in an sub
  // operation queue.
  OperationQueue operation_queue_;

  class GoogleFirebaseTokensCall;
  class GoogleOAuthTokensCall;
  class GoogleUserCredsCall;
  class GoogleRevokeTokensCall;
  class GoogleProfileAttributesCall;

  FXL_DISALLOW_COPY_AND_ASSIGN(OAuthTokenManagerApp);
};

class OAuthTokenManagerApp::TokenProviderFactoryImpl
    : modular_auth::TokenProviderFactory,
      modular_auth::TokenProvider {
 public:
  TokenProviderFactoryImpl(
      const fidl::StringPtr& account_id, OAuthTokenManagerApp* const app,
      fidl::InterfaceRequest<modular_auth::TokenProviderFactory> request)
      : account_id_(account_id), binding_(this, std::move(request)), app_(app) {
    binding_.set_error_handler(
        [this] { app_->token_provider_factory_impls_.erase(account_id_); });
  }

 private:
  // |TokenProviderFactory|
  void GetTokenProvider(
      fidl::StringPtr /*application_url*/,
      fidl::InterfaceRequest<modular_auth::TokenProvider> request) override {
    // TODO(alhaad/ukode): Current implementation is agnostic about which
    // agent is requesting what token. Fix this.
    token_provider_bindings_.AddBinding(this, std::move(request));
  }

  // |TokenProvider|
  void GetAccessToken(GetAccessTokenCallback callback) override {
    FXL_DCHECK(app_);
    app_->RefreshToken(account_id_, ACCESS_TOKEN, callback);
  }

  // |TokenProvider|
  void GetIdToken(GetIdTokenCallback callback) override {
    FXL_DCHECK(app_);
    app_->RefreshToken(account_id_, ID_TOKEN, callback);
  }

  // |TokenProvider|
  void GetFirebaseAuthToken(fidl::StringPtr firebase_api_key,
                            GetFirebaseAuthTokenCallback callback) override {
    FXL_DCHECK(app_);

    // Oauth id token is used as input to fetch firebase auth token.
    GetIdToken(
        [this, firebase_api_key = firebase_api_key, callback](
            const std::string id_token, const modular_auth::AuthErr auth_err) {
          if (auth_err.status != modular_auth::Status::OK) {
            FXL_LOG(ERROR) << "Error in refreshing Idtoken.";
            callback(nullptr, std::move(auth_err));
            return;
          }

          app_->RefreshFirebaseToken(account_id_, firebase_api_key, id_token,
                                     callback);
        });
  }

  // |TokenProvider|
  void GetClientId(GetClientIdCallback callback) override {
    callback(kClientId);
  }

  std::string account_id_;
  fidl::Binding<modular_auth::TokenProviderFactory> binding_;
  fidl::BindingSet<modular_auth::TokenProvider> token_provider_bindings_;

  OAuthTokenManagerApp* const app_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TokenProviderFactoryImpl);
};

class OAuthTokenManagerApp::GoogleFirebaseTokensCall
    : public Operation<modular_auth::FirebaseTokenPtr, modular_auth::AuthErr> {
 public:
  GoogleFirebaseTokensCall(std::string account_id, std::string firebase_api_key,
                           std::string id_token,
                           OAuthTokenManagerApp* const app,
                           FirebaseTokenCallback callback)
      : Operation("OAuthTokenManagerApp::GoogleFirebaseTokensCall", callback),
        account_id_(std::move(account_id)),
        firebase_api_key_(std::move(firebase_api_key)),
        id_token_(std::move(id_token)),
        app_(app) {}

 private:
  void Run() override {
    FlowToken flow{this, &firebase_token_, &auth_err_};

    if (account_id_.empty()) {
      Failure(flow, modular_auth::Status::BAD_REQUEST, "Account id is empty");
      return;
    }

    if (firebase_api_key_.empty()) {
      Failure(flow, modular_auth::Status::BAD_REQUEST,
              "Firebase Api key is empty");
      return;
    }

    if (id_token_.empty()) {
      // TODO(ukode): Need to differentiate between deleted users, users that
      // are not provisioned and Guest mode users. For now, return empty
      // response in such cases as there is no clear way to differentiate
      // between regular users and guest users.
      Success(flow);
      return;
    }

    // check cache for existing firebase tokens.
    bool cacheValid = IsCacheValid();
    if (!cacheValid) {
      FetchFirebaseToken(flow);
    } else {
      Success(flow);
    }
  }

  // Fetch fresh firebase auth token by exchanging idToken from Google.
  void FetchFirebaseToken(FlowToken flow) {
    FXL_DCHECK(!id_token_.empty());
    FXL_DCHECK(!firebase_api_key_.empty());

    // JSON post request body
    const std::string json_request_body =
        R"({  "postBody": "id_token=)" + id_token_ +
        "&providerId=google.com\"," + "   \"returnIdpCredential\": true," +
        "   \"returnSecureToken\": true," +
        R"(   "requestUri": "http://localhost")" + "}";

    app_->startup_context_->ConnectToEnvironmentService(
        http_service_.NewRequest());
    http_service_->CreateURLLoader(url_loader_.NewRequest());

    std::string url(kFirebaseAuthEndpoint);
    url += "?key=" + UrlEncode(firebase_api_key_);

    // This flow branches below, so we need to put it in a shared
    // container from which it can be removed once for all branches.
    FlowTokenHolder branch{flow};

    Post(json_request_body, url_loader_.get(), url,
         [this, branch] {
           std::unique_ptr<FlowToken> flow = branch.Continue();
           FXL_CHECK(flow);
           Success(*flow);
         },
         [this, branch](const modular_auth::Status status,
                        const std::string error_message) {
           std::unique_ptr<FlowToken> flow = branch.Continue();
           FXL_CHECK(flow);
           Failure(*flow, status, error_message);
         },
         [this](rapidjson::Document doc) {
           return GetFirebaseToken(std::move(doc));
         });
  }

  // Parses firebase jwt auth token from firebase auth endpoint response and
  // saves it to local token in-memory cache.
  bool GetFirebaseToken(rapidjson::Document jwt_token) {
    FXL_VLOG(1) << "Firebase Token: " << JsonValueToPrettyString(jwt_token);

    if (!jwt_token.HasMember("idToken") || !jwt_token.HasMember("localId") ||
        !jwt_token.HasMember("email") || !jwt_token.HasMember("expiresIn")) {
      FXL_LOG(ERROR)
          << "Firebase Token returned from server is missing "
          << "either idToken or email or localId fields. Returned token: "
          << JsonValueToPrettyString(jwt_token);
      return false;
    }

    uint64_t expiresIn;
    std::istringstream(jwt_token["expiresIn"].GetString()) >> expiresIn;

    app_->oauth_tokens_[account_id_].fb_tokens_[firebase_api_key_] = {
        static_cast<uint64_t>(fxl::TimePoint::Now().ToEpochDelta().ToSeconds()),
        expiresIn,
        jwt_token["idToken"].GetString(),
        jwt_token["localId"].GetString(),
        jwt_token["email"].GetString(),
    };
    return true;
  }

  // Returns true if the firebase tokens stored in cache are still valid and
  // not expired.
  bool IsCacheValid() {
    FXL_DCHECK(app_);
    FXL_DCHECK(!account_id_.empty());
    FXL_DCHECK(!firebase_api_key_.empty());

    if (app_->oauth_tokens_[account_id_].fb_tokens_.find(firebase_api_key_) ==
        app_->oauth_tokens_[account_id_].fb_tokens_.end()) {
      FXL_VLOG(1) << "Firebase api key: [" << firebase_api_key_
                  << "] not found in cache.";
      return false;
    }

    uint64_t current_ts = fxl::TimePoint::Now().ToEpochDelta().ToSeconds();
    auto fb_token =
        app_->oauth_tokens_[account_id_].fb_tokens_[firebase_api_key_];
    uint64_t creation_ts = fb_token.creation_ts;
    uint64_t token_expiry = fb_token.expires_in;
    if ((current_ts - creation_ts) <
        (token_expiry - kPaddingForTokenExpiryInS)) {
      FXL_VLOG(1) << "Returning firebase token for api key ["
                  << firebase_api_key_ << "] from cache. ";
      return true;
    }

    return false;
  }

  void Success(FlowToken /*flow*/) {
    // Set firebase token
    firebase_token_ = modular_auth::FirebaseToken::New();
    if (id_token_.empty()) {
      firebase_token_->id_token = "";
      firebase_token_->local_id = "";
      firebase_token_->email = "";
    } else {
      auto fb_token =
          app_->oauth_tokens_[account_id_].fb_tokens_[firebase_api_key_];
      firebase_token_->id_token = fb_token.id_token;
      firebase_token_->local_id = fb_token.local_id;
      firebase_token_->email = fb_token.email;
    }

    // Set status to success
    auth_err_.status = modular_auth::Status::OK;
    auth_err_.message = "";
  }

  void Failure(FlowToken /*flow*/, const modular_auth::Status& status,
               const std::string& error_message) {
    FXL_LOG(ERROR) << "Failed with error status:" << status
                   << " ,and message:" << error_message;
    auth_err_.status = status;
    auth_err_.message = error_message;
  }

  const std::string account_id_;
  const std::string firebase_api_key_;
  const std::string id_token_;
  OAuthTokenManagerApp* const app_;

  modular_auth::FirebaseTokenPtr firebase_token_;
  modular_auth::AuthErr auth_err_;

  http::HttpServicePtr http_service_;
  http::URLLoaderPtr url_loader_;

  FXL_DISALLOW_COPY_AND_ASSIGN(GoogleFirebaseTokensCall);
};

class OAuthTokenManagerApp::GoogleOAuthTokensCall
    : public Operation<fidl::StringPtr, modular_auth::AuthErr> {
 public:
  GoogleOAuthTokensCall(std::string account_id, const TokenType& token_type,
                        OAuthTokenManagerApp* const app,
                        ShortLivedTokenCallback callback)
      : Operation("OAuthTokenManagerApp::GoogleOAuthTokensCall", callback),
        account_id_(std::move(account_id)),
        token_type_(token_type),
        app_(app) {}

 private:
  void Run() override {
    FlowToken flow{this, &result_, &auth_err_};

    if (account_id_.empty()) {
      Failure(flow, modular_auth::Status::BAD_REQUEST, "Account id is empty.");
      return;
    }

    FXL_VLOG(1) << "Fetching access/id tokens for Account_ID:" << account_id_;
    const std::string refresh_token = GetRefreshTokenFromCredsFile(account_id_);
    if (refresh_token.empty()) {
      // TODO(ukode): Need to differentiate between deleted users, users that
      // are not provisioned and Guest mode users. For now, return empty
      // response in such cases as there is no clear way to differentiate
      // between regular users and guest users.
      Success(flow);
      return;
    }

    bool cacheValid = IsCacheValid();
    if (!cacheValid) {
      // fetching tokens from server.
      FetchAccessAndIdToken(refresh_token, flow);
    } else {
      Success(flow);  // fetching tokens from local cache.
    }
  }

  // Fetch fresh access and id tokens by exchanging refresh token from Google
  // token endpoint.
  void FetchAccessAndIdToken(const std::string& refresh_token, FlowToken flow) {
    FXL_CHECK(!refresh_token.empty());

    const std::string request_body = "refresh_token=" + refresh_token +
                                     "&client_id=" + kClientId +
                                     "&grant_type=refresh_token";

    app_->startup_context_->ConnectToEnvironmentService(
        http_service_.NewRequest());
    http_service_->CreateURLLoader(url_loader_.NewRequest());

    // This flow exlusively branches below, so we need to put it in a shared
    // container from which it can be removed once for all branches.
    FlowTokenHolder branch{flow};

    Post(request_body, url_loader_.get(), kGoogleOAuthTokenEndpoint,
         [this, branch] {
           std::unique_ptr<FlowToken> flow = branch.Continue();
           FXL_CHECK(flow);
           Success(*flow);
         },
         [this, branch](const modular_auth::Status status,
                        const std::string error_message) {
           std::unique_ptr<FlowToken> flow = branch.Continue();
           FXL_CHECK(flow);
           Failure(*flow, status, error_message);
         },
         [this](rapidjson::Document doc) {
           return GetShortLivedTokens(std::move(doc));
         });
  }

  // Parse access and id tokens from OAUth endpoints into local token in-memory
  // cache.
  bool GetShortLivedTokens(rapidjson::Document tokens) {
    if (!tokens.HasMember("access_token")) {
      FXL_LOG(ERROR) << "Tokens returned from server does not contain "
                     << "access_token. Returned token: "
                     << JsonValueToPrettyString(tokens);
      return false;
    };

    if ((token_type_ == ID_TOKEN) && !tokens.HasMember("id_token")) {
      FXL_LOG(ERROR) << "Tokens returned from server does not contain "
                     << "id_token. Returned token: "
                     << JsonValueToPrettyString(tokens);
      return false;
    }

    // Add the token generation timestamp to |tokens| for caching.
    uint64_t creation_ts = fxl::TimePoint::Now().ToEpochDelta().ToSeconds();
    app_->oauth_tokens_[account_id_] = {
        creation_ts,
        tokens["expires_in"].GetUint64(),
        tokens["access_token"].GetString(),
        tokens["id_token"].GetString(),
        std::map<std::string, FirebaseAuthToken>(),
    };

    return true;
  }

  // Returns true if the access and idtokens stored in cache are still valid and
  // not expired.
  bool IsCacheValid() {
    FXL_DCHECK(app_);
    FXL_DCHECK(!account_id_.empty());

    if (app_->oauth_tokens_.find(account_id_) == app_->oauth_tokens_.end()) {
      FXL_VLOG(1) << "Account: [" << account_id_ << "] not found in cache.";
      return false;
    }

    uint64_t current_ts = fxl::TimePoint::Now().ToEpochDelta().ToSeconds();
    uint64_t creation_ts = app_->oauth_tokens_[account_id_].creation_ts;
    uint64_t token_expiry = app_->oauth_tokens_[account_id_].expires_in;
    if ((current_ts - creation_ts) <
        (token_expiry - kPaddingForTokenExpiryInS)) {
      FXL_VLOG(1) << "Returning access/id tokens for account [" << account_id_
                  << "] from cache. ";
      return true;
    }

    return false;
  }

  void Success(FlowToken flow) {
    if (app_->oauth_tokens_.find(account_id_) == app_->oauth_tokens_.end()) {
      // In guest mode, return empty tokens.
      result_ = "";
    } else {
      switch (token_type_) {
        case ACCESS_TOKEN:
          result_ = app_->oauth_tokens_[account_id_].access_token;
          break;
        case ID_TOKEN:
          result_ = app_->oauth_tokens_[account_id_].id_token;
          break;
        case FIREBASE_JWT_TOKEN:
          Failure(flow, modular_auth::Status::INTERNAL_ERROR,
                  "invalid token type");
      }
    }

    // Set status to success
    auth_err_.status = modular_auth::Status::OK;
    auth_err_.message = "";
  }

  void Failure(FlowToken /*flow*/, const modular_auth::Status& status,
               const std::string& error_message) {
    FXL_LOG(ERROR) << "Failed with error status:" << status
                   << " ,and message:" << error_message;
    auth_err_.status = status;
    auth_err_.message = error_message;
  }

  const std::string account_id_;
  const std::string firebase_api_key_;
  TokenType token_type_;
  OAuthTokenManagerApp* const app_;

  http::HttpServicePtr http_service_;
  http::URLLoaderPtr url_loader_;

  fidl::StringPtr result_;
  modular_auth::AuthErr auth_err_;

  FXL_DISALLOW_COPY_AND_ASSIGN(GoogleOAuthTokensCall);
};

// TODO(alhaad): Use variadic template in |Operation|. That way, parameters to
// |callback| can be returned as parameters to |Done()|.
class OAuthTokenManagerApp::GoogleUserCredsCall
    : public Operation<>,
      fuchsia::webview::WebRequestDelegate {
 public:
  GoogleUserCredsCall(modular_auth::AccountPtr account,
                      OAuthTokenManagerApp* const app,
                      AddAccountCallback callback)
      : Operation("OAuthTokenManagerApp::GoogleUserCredsCall", [] {}),
        account_(std::move(account)),
        app_(app),
        callback_(std::move(callback)) {}

 private:
  // |Operation|
  void Run() override {
    // No FlowToken used here; calling Done() directly is more suitable,
    // because of the flow of control through
    // fuchsia::webview::WebRequestDelegate.

    auto view_owner = SetupWebView();

    // Set a delegate which will parse incoming URLs for authorization code.
    // TODO(alhaad/ukode): We need to set a timout here in-case we do not get
    // the code.
    fuchsia::webview::WebRequestDelegatePtr web_request_delegate;
    web_request_delegate_bindings_.AddBinding(
        this, web_request_delegate.NewRequest());
    web_view_->SetWebRequestDelegate(std::move(web_request_delegate));

    web_view_->ClearCookies();

    const std::vector<std::string> scopes(kScopes.begin(), kScopes.end());
    std::string joined_scopes = fxl::JoinStrings(scopes, "+");

    std::string url = kGoogleOAuthAuthEndpoint;
    url += "?scope=" + joined_scopes;
    url += "&response_type=code&redirect_uri=";
    url += kRedirectUri;
    url += "&client_id=";
    url += kClientId;

    web_view_->SetUrl(url);

    app_->account_provider_context_->GetAuthenticationContext(
        account_->id, auth_context_.NewRequest());

    auth_context_.set_error_handler([this] {
      callback_(nullptr, "Overlay cancelled by device shell.");
      Done();
    });
    auth_context_->StartOverlay(std::move(view_owner));
  }

  // |fuchsia::webview::WebRequestDelegate|
  void WillSendRequest(fidl::StringPtr incoming_url) override {
    const std::string& uri = incoming_url.get();
    const std::string prefix = std::string{kRedirectUri} + "?code=";
    const std::string cancel_prefix =
        std::string{kRedirectUri} + "?error=access_denied";

    auto cancel_pos = uri.find(cancel_prefix);
    // user denied OAuth permissions
    if (cancel_pos == 0) {
      Failure(modular_auth::Status::USER_CANCELLED,
              "User cancelled OAuth flow");
      return;
    }

    auto pos = uri.find(prefix);
    // user performing gaia authentication inside webview, let it pass
    if (pos != 0) {
      return;
    }

    // user accepted OAuth permissions - close the webview and exchange auth
    // code to long lived credential.
    // Also, de-register previously registered error callbacks since calling
    // StopOverlay() might cause this connection to be closed.
    auth_context_.set_error_handler([] {});
    auth_context_->StopOverlay();

    auto code = uri.substr(prefix.size(), std::string::npos);
    // There is a '#' character at the end.
    code.pop_back();

    const std::string request_body =
        "code=" + code + "&redirect_uri=" + kRedirectUri +
        "&client_id=" + kClientId + "&grant_type=authorization_code";

    app_->startup_context_->ConnectToEnvironmentService(
        http_service_.NewRequest());
    http_service_->CreateURLLoader(url_loader_.NewRequest());

    Post(request_body, url_loader_.get(), kGoogleOAuthTokenEndpoint,
         [this] { Success(); },
         [this](const modular_auth::Status status,
                const std::string error_message) {
           Failure(status, error_message);
         },
         [this](rapidjson::Document doc) {
           return ProcessCredentials(std::move(doc));
         });
  }

  // Parses refresh tokens from auth endpoint response and persists it in
  // |kCredentialsFile|.
  bool ProcessCredentials(rapidjson::Document tokens) {
    if (!tokens.HasMember("refresh_token") ||
        !tokens.HasMember("access_token")) {
      FXL_LOG(ERROR) << "Tokens returned from server does not contain "
                     << "refresh_token or access_token. Returned token: "
                     << JsonValueToPrettyString(tokens);
      return false;
    };

    if (!SaveCredentials(tokens["refresh_token"].GetString())) {
      return false;
    }

    // Store short lived tokens local in-memory cache.
    uint64_t creation_ts = fxl::TimePoint::Now().ToEpochDelta().ToSeconds();
    app_->oauth_tokens_[account_->id] = {
        creation_ts,
        tokens["expires_in"].GetUint64(),
        tokens["access_token"].GetString(),
        tokens["id_token"].GetString(),
        std::map<std::string, FirebaseAuthToken>(),
    };
    return true;
  }

  // Saves new credentials to the persistent creds storage file.
  bool SaveCredentials(const std::string& refresh_token) {
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<::auth::UserCredential>> creds;

    const ::auth::CredentialStore* file_creds = ParseCredsFile();
    if (file_creds != nullptr) {
      // Reserialize existing users.
      for (const auto* cred : *file_creds->creds()) {
        if (cred->account_id()->str() == account_->id) {
          // Update existing credentials
          continue;
        }

        std::vector<flatbuffers::Offset<::auth::IdpCredential>> idp_creds;
        for (const auto* idp_cred : *cred->tokens()) {
          idp_creds.push_back(::auth::CreateIdpCredential(
              builder, idp_cred->identity_provider(),
              builder.CreateString(idp_cred->refresh_token())));
        }

        creds.push_back(::auth::CreateUserCredential(
            builder, builder.CreateString(cred->account_id()),
            builder.CreateVector<flatbuffers::Offset<::auth::IdpCredential>>(
                idp_creds)));
      }
    }

    // add the new credential for |account_->id|.
    std::vector<flatbuffers::Offset<::auth::IdpCredential>> new_idp_creds;
    new_idp_creds.push_back(
        ::auth::CreateIdpCredential(builder, ::auth::IdentityProvider_GOOGLE,
                                    builder.CreateString(refresh_token)));

    creds.push_back(::auth::CreateUserCredential(
        builder, builder.CreateString(account_->id),
        builder.CreateVector<flatbuffers::Offset<::auth::IdpCredential>>(
            new_idp_creds)));

    builder.Finish(
        ::auth::CreateCredentialStore(builder, builder.CreateVector(creds)));

    std::string new_serialized_creds = std::string(
        reinterpret_cast<const char*>(builder.GetCurrentBufferPointer()),
        builder.GetSize());

    // Add new credentials to in-memory cache |creds_|.
    app_->creds_ = ::auth::GetCredentialStore(new_serialized_creds.data());

    return WriteCredsFile(new_serialized_creds);
  }

  void Success() {
    callback_(std::move(account_), nullptr);
    Done();
  }

  void Failure(const modular_auth::Status& status,
               const std::string& error_message) {
    FXL_LOG(ERROR) << "Failed with error status:" << status
                   << " ,and message:" << error_message;
    callback_(nullptr, error_message);
    auth_context_.set_error_handler([] {});
    auth_context_->StopOverlay();
    Done();
  }

  fuchsia::ui::views_v1_token::ViewOwnerPtr SetupWebView() {
    fuchsia::sys::Services web_view_services;
    fuchsia::sys::LaunchInfo web_view_launch_info;
    web_view_launch_info.url = kWebViewUrl;
    web_view_launch_info.directory_request = web_view_services.NewRequest();
    app_->startup_context_->launcher()->CreateComponent(
        std::move(web_view_launch_info), web_view_controller_.NewRequest());
    web_view_controller_.set_error_handler([this] {
      FXL_CHECK(false) << "web_view not found at " << kWebViewUrl << ".";
    });

    fuchsia::ui::views_v1_token::ViewOwnerPtr view_owner;
    fuchsia::ui::views_v1::ViewProviderPtr view_provider;
    web_view_services.ConnectToService(view_provider.NewRequest());
    fuchsia::sys::ServiceProviderPtr web_view_moz_services;
    view_provider->CreateView(view_owner.NewRequest(),
                              web_view_moz_services.NewRequest());

    ConnectToService(web_view_moz_services.get(), web_view_.NewRequest());

    return view_owner;
  }

  modular_auth::AccountPtr account_;
  OAuthTokenManagerApp* const app_;
  const AddAccountCallback callback_;

  modular_auth::AuthenticationContextPtr auth_context_;

  fuchsia::webview::WebViewPtr web_view_;
  fuchsia::sys::ComponentControllerPtr web_view_controller_;

  http::HttpServicePtr http_service_;
  http::URLLoaderPtr url_loader_;

  fidl::BindingSet<fuchsia::webview::WebRequestDelegate>
      web_request_delegate_bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(GoogleUserCredsCall);
};

class OAuthTokenManagerApp::GoogleRevokeTokensCall
    : public Operation<modular_auth::AuthErr> {
 public:
  GoogleRevokeTokensCall(modular_auth::AccountPtr account, bool revoke_all,
                         OAuthTokenManagerApp* const app,
                         RemoveAccountCallback callback)
      : Operation("OAuthTokenManagerApp::GoogleRevokeTokensCall", callback),
        account_(std::move(account)),
        revoke_all_(revoke_all),
        app_(app),
        callback_(callback) {}

 private:
  // |Operation|
  void Run() override {
    FlowToken flow{this, &auth_err_};

    if (!account_) {
      Failure(flow, modular_auth::Status::BAD_REQUEST, "Account is null.");
      return;
    }

    switch (account_->identity_provider) {
      case modular_auth::IdentityProvider::DEV:
        Success(flow);  // guest mode
        return;
      case modular_auth::IdentityProvider::GOOGLE:
        break;
      default:
        Failure(flow, modular_auth::Status::BAD_REQUEST, "Unsupported IDP.");
        return;
    }

    const std::string refresh_token =
        GetRefreshTokenFromCredsFile(account_->id);
    if (refresh_token.empty()) {
      FXL_LOG(ERROR) << "Account: " << account_->id << " not found.";
      Success(flow);  // Maybe a guest account.
      return;
    }

    // delete local cache first.
    if (app_->oauth_tokens_.find(account_->id) != app_->oauth_tokens_.end()) {
      if (!app_->oauth_tokens_.erase(account_->id)) {
        Failure(flow, modular_auth::Status::INTERNAL_ERROR,
                "Unable to delete cached tokens for account:" +
                    std::string(account_->id));
        return;
      }
    }

    // delete user credentials from local persistent storage.
    if (!DeleteCredentials()) {
      Failure(flow, modular_auth::Status::INTERNAL_ERROR,
              "Unable to delete persistent credentials for account:" +
                  std::string(account_->id));
      return;
    }

    if (!revoke_all_) {
      Success(flow);
      return;
    }

    // revoke persistent tokens on backend IDP server.
    app_->startup_context_->ConnectToEnvironmentService(
        http_service_.NewRequest());
    http_service_->CreateURLLoader(url_loader_.NewRequest());

    std::string url = kGoogleRevokeTokenEndpoint + std::string("?token=");
    url += refresh_token;

    std::string request_body;

    // This flow branches below, so we need to put it in a shared container
    // from which it can be removed once for all branches.
    FlowTokenHolder branch{flow};

    Post(request_body, url_loader_.get(), url,
         [this, branch] {
           std::unique_ptr<FlowToken> flow = branch.Continue();
           FXL_CHECK(flow);
           Success(*flow);
         },
         [this, branch](const modular_auth::Status status,
                        const std::string error_message) {
           std::unique_ptr<FlowToken> flow = branch.Continue();
           FXL_CHECK(flow);
           Failure(*flow, status, error_message);
         },
         [this](rapidjson::Document doc) {
           return RevokeAllTokens(std::move(doc));
         });
  }

  // Deletes existing user credentials for |account_->id|.
  bool DeleteCredentials() {
    const ::auth::CredentialStore* credentials_storage = ParseCredsFile();
    if (credentials_storage == nullptr) {
      FXL_LOG(ERROR) << "Failed to parse credentials.";
      return false;
    }

    // Delete |account_->id| credentials and reserialize existing users.
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<::auth::UserCredential>> creds;

    for (const auto* cred : *credentials_storage->creds()) {
      if (cred->account_id()->str() == account_->id) {
        // delete existing credentials
        continue;
      }

      std::vector<flatbuffers::Offset<::auth::IdpCredential>> idp_creds;
      for (const auto* idp_cred : *cred->tokens()) {
        idp_creds.push_back(::auth::CreateIdpCredential(
            builder, idp_cred->identity_provider(),
            builder.CreateString(idp_cred->refresh_token())));
      }

      creds.push_back(::auth::CreateUserCredential(
          builder, builder.CreateString(cred->account_id()),
          builder.CreateVector<flatbuffers::Offset<::auth::IdpCredential>>(
              idp_creds)));
    }

    builder.Finish(
        ::auth::CreateCredentialStore(builder, builder.CreateVector(creds)));

    std::string new_serialized_creds = std::string(
        reinterpret_cast<const char*>(builder.GetCurrentBufferPointer()),
        builder.GetSize());

    // Add updated credentials to in-memory cache |creds_|.
    app_->creds_ = ::auth::GetCredentialStore(new_serialized_creds.data());

    return WriteCredsFile(new_serialized_creds);
  }

  // Invalidate both refresh and access tokens on backend IDP server.
  // If the revocation is successfully processed, then the status code of the
  // response is 200. For error conditions, a status code 400 is returned along
  // with an error code in the response body.
  bool RevokeAllTokens(rapidjson::Document status) {
    FXL_VLOG(1) << "Revoke token api response: "
                << JsonValueToPrettyString(status);

    return true;
  }

  void Success(FlowToken /*flow*/) {
    // Set status to success
    auth_err_.status = modular_auth::Status::OK;
    auth_err_.message = "";
  }

  void Failure(FlowToken /*flow*/, const modular_auth::Status& status,
               const std::string& error_message) {
    FXL_LOG(ERROR) << "Failed with error status:" << status
                   << " ,and message:" << error_message;
    auth_err_.status = status;
    auth_err_.message = error_message;
  }

  modular_auth::AccountPtr account_;
  // By default, RemoveAccount deletes account only from the device where the
  // user performed the operation.
  bool revoke_all_ = false;
  OAuthTokenManagerApp* const app_;
  const RemoveAccountCallback callback_;

  http::HttpServicePtr http_service_;
  http::URLLoaderPtr url_loader_;

  modular_auth::AuthErr auth_err_;

  FXL_DISALLOW_COPY_AND_ASSIGN(GoogleRevokeTokensCall);
};

class OAuthTokenManagerApp::GoogleProfileAttributesCall : public Operation<> {
 public:
  GoogleProfileAttributesCall(modular_auth::AccountPtr account,
                              OAuthTokenManagerApp* const app,
                              AddAccountCallback callback)
      : Operation("OAuthTokenManagerApp::GoogleProfileAttributesCall", [] {}),
        account_(std::move(account)),
        app_(app),
        callback_(std::move(callback)) {}

 private:
  // |Operation|
  void Run() override {
    if (!account_) {
      Failure(modular_auth::Status::BAD_REQUEST, "Account is null.");
      return;
    }

    if (app_->oauth_tokens_.find(account_->id) == app_->oauth_tokens_.end()) {
      FXL_LOG(ERROR) << "Account: " << account_->id << " not found.";
      Success();  // Maybe a guest account.
      return;
    }

    const std::string access_token =
        app_->oauth_tokens_[account_->id].access_token;
    app_->startup_context_->ConnectToEnvironmentService(
        http_service_.NewRequest());
    http_service_->CreateURLLoader(url_loader_.NewRequest());

    // Fetch profile atrributes for the provisioned user using
    // https://developers.google.com/+/web/api/rest/latest/people/get api.
    Get(url_loader_.get(), kGooglePeopleGetEndpoint, access_token,
        [this] { Success(); },
        [this](const modular_auth::Status status,
               const std::string error_message) {
          Failure(status, error_message);
        },
        [this](rapidjson::Document doc) {
          return SetAccountAttributes(std::move(doc));
        });
  }

  // Populate profile urls and display name for the account.
  bool SetAccountAttributes(rapidjson::Document attributes) {
    FXL_VLOG(1) << "People:get api response: "
                << JsonValueToPrettyString(attributes);

    if (!account_) {
      return false;
    }

    if (attributes.HasMember("displayName")) {
      account_->display_name = attributes["displayName"].GetString();
    }
    if (account_->display_name.is_null()) {
      account_->display_name = "";
    }

    if (attributes.HasMember("url")) {
      account_->url = attributes["url"].GetString();
    } else {
      account_->url = "";
    }

    if (attributes.HasMember("image")) {
      account_->image_url = attributes["image"]["url"].GetString();
    } else {
      account_->image_url = "";
    }

    return true;
  }

  void Success() {
    callback_(std::move(account_), nullptr);
    Done();
  }

  void Failure(const modular_auth::Status& status,
               const std::string& error_message) {
    FXL_LOG(ERROR) << "Failed with error status:" << status
                   << " ,and message:" << error_message;

    // Account is missing profile attributes, but still valid.
    callback_(std::move(account_), error_message);
    Done();
  }

  modular_auth::AccountPtr account_;
  OAuthTokenManagerApp* const app_;
  const AddAccountCallback callback_;

  http::HttpServicePtr http_service_;
  http::URLLoaderPtr url_loader_;

  FXL_DISALLOW_COPY_AND_ASSIGN(GoogleProfileAttributesCall);
};

OAuthTokenManagerApp::OAuthTokenManagerApp(async::Loop* loop)
    : loop_(loop),
      startup_context_(fuchsia::sys::StartupContext::CreateFromStartupInfo()),
      binding_(this) {
  startup_context_->outgoing().AddPublicService<AccountProvider>(
      [this](fidl::InterfaceRequest<AccountProvider> request) {
        binding_.Bind(std::move(request));
      });
  // Reserialize existing users.
  if (files::IsFile(kCredentialsFile)) {
    creds_ = ParseCredsFile();
    if (creds_ == nullptr) {
      FXL_LOG(WARNING) << "Error in parsing existing credentials from: "
                       << kCredentialsFile;
    }
  }
}

void OAuthTokenManagerApp::Initialize(
    fidl::InterfaceHandle<modular_auth::AccountProviderContext> provider) {
  FXL_VLOG(1) << "OAuthTokenManagerApp::Initialize()";
  account_provider_context_.Bind(std::move(provider));
}

void OAuthTokenManagerApp::Terminate() {
  FXL_LOG(INFO) << "OAuthTokenManagerApp::Terminate()";
  loop_->Quit();
}

// TODO(alhaad): Check if account id already exists.
std::string OAuthTokenManagerApp::GenerateAccountId() {
  uint32_t random_number;
  size_t random_size;
  zx_status_t status =
      zx_cprng_draw(&random_number, sizeof random_number, &random_size);
  FXL_CHECK(status == ZX_OK);
  FXL_CHECK(sizeof random_number == random_size);
  return std::to_string(random_number);
}

void OAuthTokenManagerApp::AddAccount(
    modular_auth::IdentityProvider identity_provider,
    AddAccountCallback callback) {
  FXL_VLOG(1) << "OAuthTokenManagerApp::AddAccount()";
  auto account = modular_auth::Account::New();
  account->id = GenerateAccountId();
  account->identity_provider = identity_provider;
  account->display_name = "";
  account->url = "";
  account->image_url = "";

  switch (identity_provider) {
    case modular_auth::IdentityProvider::DEV:
      callback(std::move(account), nullptr);
      return;
    case modular_auth::IdentityProvider::GOOGLE:
      operation_queue_.Add(new GoogleUserCredsCall(
          std::move(account), this,
          [this, callback](modular_auth::AccountPtr account,
                           const fidl::StringPtr error_msg) {
            if (error_msg) {
              callback(nullptr, error_msg);
              return;
            }

            operation_queue_.Add(new GoogleProfileAttributesCall(
                std::move(account), this, callback));
          }));
      return;
    default:
      callback(nullptr, "Unrecognized Identity Provider");
  }
}

void OAuthTokenManagerApp::RemoveAccount(modular_auth::Account account,
                                         bool revoke_all,
                                         RemoveAccountCallback callback) {
  FXL_VLOG(1) << "OAuthTokenManagerApp::RemoveAccount()";
  operation_queue_.Add(new GoogleRevokeTokensCall(
      fidl::MakeOptional(std::move(account)), revoke_all, this, callback));
}

void OAuthTokenManagerApp::GetTokenProviderFactory(
    fidl::StringPtr account_id,
    fidl::InterfaceRequest<modular_auth::TokenProviderFactory> request) {
  new TokenProviderFactoryImpl(account_id, this, std::move(request));
}

void OAuthTokenManagerApp::RefreshToken(const std::string& account_id,
                                        const TokenType& token_type,
                                        ShortLivedTokenCallback callback) {
  FXL_VLOG(1) << "OAuthTokenManagerApp::RefreshToken()";
  operation_queue_.Add(
      new GoogleOAuthTokensCall(account_id, token_type, this, callback));
}

void OAuthTokenManagerApp::RefreshFirebaseToken(
    const std::string& account_id, const std::string& firebase_api_key,
    const std::string& id_token, FirebaseTokenCallback callback) {
  FXL_VLOG(1) << "OAuthTokenManagerApp::RefreshFirebaseToken()";
  operation_queue_.Add(new GoogleFirebaseTokensCall(
      account_id, firebase_api_key, id_token, this, callback));
}

}  // namespace auth
}  // namespace modular
}  // namespace fuchsia

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line)) {
    return 1;
  }

  async::Loop loop(&kAsyncLoopConfigMakeDefault);
  trace::TraceProvider trace_provider(loop.async());

  fuchsia::modular::auth::OAuthTokenManagerApp app(&loop);
  loop.Run();
  return 0;
}
