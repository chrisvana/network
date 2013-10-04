// Copyright 2013
// Author: Christopher Van Arsdale

#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "common/base/init.h"
#include "common/log/log.h"
#include "third_party/network/http.h"
#include <curl/curl.h>

REGISTER_MODULE_INITIALIZER(curl, {
    curl_global_init(CURL_GLOBAL_DEFAULT);
  });

using std::string;
using std::vector;

namespace network {
namespace {
struct PutData {
  PutData(const string& d) : data(&d), offset(0) {}
  const string* data;
  size_t offset;
};
static size_t curl_is_stupid_put_callback(void* data, size_t size,
                                          size_t nmemb, void* userp) {
  PutData* d = static_cast<PutData*>(userp);
  CHECK(d);
  size_t requested = size * nmemb;
  size_t remaining = d->data->size() - d->offset;
  size_t actual = std::min(requested, remaining);
  memcpy(data, d->data->data() + d->offset, actual);
  d->offset += actual;
  return actual;
}

static size_t curl_is_still_stupid_response_callback(
    void* contents, size_t size, size_t nmemb, void *userp) {
  size_t bytesize = size * nmemb;
  string* out = static_cast<string*>(userp);
  size_t current = out->size();

  // TODO(cvanarsdale): byte limit.
  out->resize(current + bytesize);
  memcpy(&(*out)[current], contents, bytesize);
  return bytesize;
}

}  // anonymous namespace

class HTTPConnection::CurlFreelist {
 public:
  CurlFreelist(int max_size) : max_size_(max_size) {}
  ~CurlFreelist() { ResizeFreelist(0); }

  class ScopedCurl {
   public:
    ScopedCurl(CurlFreelist* parent, CURL* curl)
        : parent_(parent),
          curl_(curl) {}
    ~ScopedCurl() { parent_->Release(curl_); }
    CURL* get() { return curl_; }
    
   private:
    CurlFreelist* parent_;
    CURL* curl_;
  };

  CURL* NewCurl();
  void Release(CURL* curl);

 private:
  void ResizeFreelist(int size);

  const int max_size_;
  Mutex lock_;
  vector<CURL*> free_curls_;
};

string HTTPRequest::DebugString() const {
  string out;
  switch (type_) {
    case HTTPRequest::GET:
      out = "GET ";
      break;
    case HTTPRequest::POST:
      out = "POST ";
      break;
    case HTTPRequest::PUT:
      out = "PUT ";
      break;
    case HTTPRequest::DELETE:
      out = "DELETE ";
      break;
    default:
      out = "??INVALID?? ";
      break;
  }
  out.append(url_.path());
  for (const auto& it : headers_.headers()) {
    out += "\n" + it.first + ": " + it.second;
  }
  if (!content_.empty()) {
    out.append("\n\n");
    out.append(content_);
  }
  return out;
}

HTTPResponse::HTTPResponse()
    : response_code_(-1) {
}

HTTPResponse::~HTTPResponse() {
}

static const int kMaxFreelistSize = 10;  // TODO(cvanarsdale): parameter.
HTTPConnection::HTTPConnection(const HTTPConnection::Options& options)
    : options_(options),
      curl_freelist_(new CurlFreelist(kMaxFreelistSize)) {
  // just to check;
  CurlFreelist::ScopedCurl curl(curl_freelist_.get(),
                                curl_freelist_->NewCurl());
}

HTTPConnection::~HTTPConnection() {
}

HTTPResponse* HTTPConnection::BlockingRequest(const HTTPRequest& request) {
  // Some input caching.
  PutData put_data(request.content());
  string output;

  // Set up curl
  CurlFreelist::ScopedCurl curl(curl_freelist_.get(),
                                curl_freelist_->NewCurl());
  if (curl.get() == NULL) {
    LOG(ERROR) << "Could not initialize CURL.";
    return NULL;
  }
  curl_easy_setopt(curl.get(), CURLOPT_URL, request.url().path().c_str());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
                   curl_is_still_stupid_response_callback);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, static_cast<void*>(&output));
  switch (request.request_type()) {
    case HTTPRequest::GET:
      break;
    case HTTPRequest::POST:
      curl_easy_setopt(curl.get(), CURLOPT_POST, 1);
      curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS,
                       request.content().c_str());
      curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                       request.content().size());
      break;
    case HTTPRequest::PUT:
      curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1);  // same as PUT.
      curl_easy_setopt(curl.get(), CURLOPT_PUT, 1);
      curl_easy_setopt(curl.get(), CURLOPT_INFILESIZE,
                       request.content().size());
      curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION,
                       curl_is_stupid_put_callback);
      curl_easy_setopt(curl.get(), CURLOPT_READDATA,
                       static_cast<void*>(&put_data));
      break;
    case HTTPRequest::DELETE:
      curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "DELETE");
      break;
    default:
      LOG(FATAL) << "Invalid request type: " << request.request_type();
      return NULL;
  }

  // Set our headers
  struct curl_slist *headers = NULL;
  for (auto it : request.headers().headers()) {
    headers = curl_slist_append(headers, (it.first + ": " + it.second).c_str());
  }
  if (headers != NULL) {
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
  }

  // Send the request
  CURLcode curl_result = curl_easy_perform(curl.get());
  if (curl_result != CURLE_OK) {
    LOG(ERROR) << "CURL failed: " << curl_easy_strerror(curl_result);
    return NULL;
  }

  std::unique_ptr<HTTPResponse> response(new HTTPResponse());
  response->mutable_content()->swap(output);
  long response_code;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response_code);
  response->set_response_code(response_code);
  return response.release();
}

void HTTPConnection::CurlFreelist::ResizeFreelist(int size) {
  MutexLock l(&lock_);
  while (free_curls_.size() > size) {
    CURL* curl = free_curls_.back();
    free_curls_.resize(free_curls_.size() - 1);
    lock_.Unlock();
    curl_easy_cleanup(curl);
    lock_.Lock();
  }
}

CURL* HTTPConnection::CurlFreelist::NewCurl() {
  {  // First check cache
    MutexLock l(&lock_);
    if (!free_curls_.empty()) {
      CURL* curl = free_curls_.back();
      free_curls_.resize(free_curls_.size() - 1);
      return curl;
    }
  }

  CURL* curl = curl_easy_init();
  if (curl == NULL) {
    LOG(ERROR) << "Could not intialize CURL.";
  }
  return curl;
}

void HTTPConnection::CurlFreelist::Release(CURL* curl) {
  if (curl != NULL) {
    curl_easy_reset(curl);  // TODO, ok if we call curl_easy_cleanup() too?

    MutexLock l(&lock_);
    if (free_curls_.size() < max_size_) {
      free_curls_.push_back(curl);
      curl = NULL;
    }
  }
  if (curl != NULL) {
    curl_easy_cleanup(curl);
  }
}

}  // namespace network
