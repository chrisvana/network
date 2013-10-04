// Copyright 2013
// Author: Christopher Van Arsdale

#ifndef _COMPOUNDS_NETWORK_HTTP_H__
#define _COMPOUNDS_NETWORK_HTTP_H__

#include <map>
#include <memory>
#include <string>
#include "common/base/macros.h"
#include "common/base/mutex.h"

namespace network {

class URL {
 public:
  // TODO(cvanarsdale): Real class with error checking, domain/tld parsing, etc.
  URL() {}
  explicit URL(const std::string& path) : path_(path) {}
  ~URL() {}

  bool IsValid() const { return !path_.empty(); }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

class HTTPHeaders {
 public:
  HTTPHeaders() {}
  ~HTTPHeaders() {}

  // Mutators.
  void Set(const std::string& key, const std::string& value) {
    headers_[key] = value;
  }
  void Erase(const std::string& key) { headers_.erase(key); }

  // Accessors
  const std::map<std::string, std::string>& headers() const { return headers_; }
  const std::string& Get(const std::string& key) const {
    static std::string kEmpty;
    std::map<std::string, std::string>::const_iterator it = headers_.find(key);
    return (it == headers_.end() ? kEmpty : it->second);
  }

 private:
  std::map<std::string, std::string> headers_;
};

class HTTPRequest {
 public:
  enum Type {
    GET,
    POST,
    PUT,
    DELETE,
  };

  HTTPRequest() : type_(GET) {}
  explicit HTTPRequest(const URL& url) : type_(GET), url_(url) {}
  ~HTTPRequest() {}

  // Mutators
  URL* mutable_url() { return &url_; }
  void SetUrl(const URL& url) { url_ = url; }
  void SetRequestType(Type type) { type_ = type; }
  HTTPHeaders* mutable_headers() { return &headers_; }
  void SetMimeType(const std::string& encoding) {
    mutable_headers()->Set("Content-Type", encoding);
  }
  void SetContent(const std::string& content) { content_ = content; }

  // Accessors
  const URL& url() const { return url_; }
  Type request_type() const { return type_; }
  const HTTPHeaders& headers() const { return headers_; }
  const std::string& content() const { return content_; }

  std::string DebugString() const;

 private:
  Type type_;
  URL url_;
  HTTPHeaders headers_;
  std::string content_;
};

class HTTPResponse {
 public:
  HTTPResponse();
  ~HTTPResponse();

  // Mutators
  void set_response_code(int code) { response_code_ = code; }
  void set_content(const std::string& content) { content_ = content; }
  std::string* mutable_content() { return &content_; }
  HTTPHeaders* mutable_headers() { return &headers_; }

  // Accessors
  int response_code() const { return response_code_; }
  const std::string& content() const { return content_; }
  const HTTPHeaders& headers() const { return headers_; }

 private:
  std::string content_;
  int response_code_;
  HTTPHeaders headers_;
};

class HTTPConnection_Options {
 public:
  HTTPConnection_Options() {}
  ~HTTPConnection_Options() {}
  // TODO.
};

class HTTPConnection {
 public:
  typedef HTTPConnection_Options Options;
  explicit HTTPConnection(const Options& options);
  ~HTTPConnection();

  HTTPResponse* BlockingRequest(const HTTPRequest& request);

  // TODO(cvanarsdale): Async callback.
  // TODO(cvanarsdale): Maintain connection

 private:
  DISALLOW_COPY_AND_ASSIGN(HTTPConnection);

  Options options_;
  class CurlFreelist;
  std::unique_ptr<CurlFreelist> curl_freelist_;
};

}  // namespace network

#endif
