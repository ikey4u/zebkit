// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_TEST_MOCK_REDUCE_ACCEPT_LANGUAGE_CONTROLLER_DELEGATE_H_
#define CONTENT_TEST_MOCK_REDUCE_ACCEPT_LANGUAGE_CONTROLLER_DELEGATE_H_

#include "zebkit/engine/content_names.h"
#include <map>

#include "content/public/browser/reduce_accept_language_controller_delegate.h"
#include "url/origin.h"

namespace zebkit {

class AcceptLanguageDelegate
    : public ReduceAcceptLanguageControllerDelegate {
 public:
  // Initialize user's accept-languages list as `languages` in the mock
  // instance, which is similar to setting user's Prefs accept-language list.
  explicit AcceptLanguageDelegate(
      const std::string& languages,
      bool is_incognito = false);

  AcceptLanguageDelegate(
      const AcceptLanguageDelegate&) = delete;
  AcceptLanguageDelegate& operator=(
      const AcceptLanguageDelegate&) = delete;

  ~AcceptLanguageDelegate() override;

  // ReduceAcceptLanguageControllerDelegate overrides.
  std::optional<std::string> GetReducedLanguage(
      const url::Origin& origin) override;
  std::vector<std::string> GetUserAcceptLanguages() const override;
  void PersistReducedLanguage(const url::Origin& origin,
                              const std::string& language) override;
  void ClearReducedLanguage(const url::Origin& origin) override;

  // Change the user's accept-language list for testing purpose.
  void SetUserAcceptLanguages(const std::string& languages);

 private:
  std::vector<std::string> user_accept_languages_;
  std::map<url::Origin, std::string> reduce_accept_language_map_;
  const bool is_incognito_;
};

}  // namespace zebkit

#endif  // CONTENT_TEST_MOCK_REDUCE_ACCEPT_LANGUAGE_CONTROLLER_DELEGATE_H_
