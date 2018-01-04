// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "topaz/auth_providers/oauth/oauth_request_builder.h"

#include <iomanip>
#include <iostream>

#include "lib/fsl/socket/strings.h"
#include "lib/fsl/vmo/strings.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/strings/join_strings.h"
#include "lib/fxl/strings/string_number_conversions.h"

namespace auth_providers {
namespace oauth {

namespace {

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

}  // namespace

OAuthRequestBuilder::OAuthRequestBuilder(std::string url, std::string method)
    : url_(UrlEncode(url)), method_(method) {
  FXL_CHECK(!url.empty());
  FXL_CHECK(!method.empty());
}

OAuthRequestBuilder::~OAuthRequestBuilder() {}

OAuthRequestBuilder& OAuthRequestBuilder::SetAuthorizationHeader(
    const std::string& token) {
  FXL_DCHECK(!token.empty());
  http_headers_["Authorization"] = "Bearer " + token;
  return *this;
}

OAuthRequestBuilder& OAuthRequestBuilder::SetUrlEncodedBody(
    const std::string& body) {
  http_headers_["content-type"] = "application/x-www-form-urlencoded";

  if (body.empty()) {
    return *this;
  }
  return SetRequestBody(UrlEncode(body));
}

OAuthRequestBuilder& OAuthRequestBuilder::SetJsonBody(const std::string& body) {
  http_headers_["accept"] = "application/json";
  http_headers_["content-type"] = "application/json";
  return SetRequestBody(body);
}

network::URLRequestPtr OAuthRequestBuilder::Build() const {
  fsl::SizedVmo data;
  auto result = fsl::VmoFromString(request_body_, &data);
  FXL_DCHECK(result);

  auto request = network::URLRequest::New();
  request->url = url_;
  request->method = method_;
  request->auto_follow_redirects = true;
  request->body = ::network::URLBody::New();
  request->body->set_sized_buffer(std::move(data).ToTransport());
  for (const auto& http_header : http_headers_) {
    ::network::HttpHeaderPtr hdr = ::network::HttpHeader::New();
    hdr->name = http_header.first;
    hdr->value = http_header.second;
    request->headers.push_back(std::move(hdr));
  }

  return request;
}

OAuthRequestBuilder& OAuthRequestBuilder::SetRequestBody(
    const std::string& body) {
  request_body_ = body;

  uint64_t data_size = request_body_.length();
  if (data_size > 0)
    http_headers_["content-length"] = fxl::NumberToString(data_size).data();

  return *this;
}

}  // namespace oauth
}  // namespace auth_providers
