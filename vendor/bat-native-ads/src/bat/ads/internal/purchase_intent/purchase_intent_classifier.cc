/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bat/ads/internal/purchase_intent/purchase_intent_classifier.h"

#include <stdint.h>
#include <functional>
#include <utility>
#include <algorithm>

#include "base/json/json_reader.h"
#include "base/time/time.h"
#include "bat/ads/internal/time_util.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"
#include "third_party/re2/src/re2/re2.h"

namespace ads {

constexpr u_int16_t kExpectedPurchaseIntentModelVersion = 1;
constexpr uint16_t kPurchaseIntentDefaultSignalWeight = 1;
constexpr uint16_t kPurchaseIntentWordCountLimit = 1000;

PurchaseIntentClassifier::PurchaseIntentClassifier() {
}

PurchaseIntentClassifier::~PurchaseIntentClassifier() = default;

bool PurchaseIntentClassifier::IsInitialized() {
  return is_initialized_;
}

bool PurchaseIntentClassifier::Initialize(
    const std::string& json) {

  is_initialized_ = FromJson(json);
  return is_initialized_;
}

PurchaseIntentSignalInfo PurchaseIntentClassifier::ExtractIntentSignal(
    const std::string& url) {
  PurchaseIntentSignalInfo signal_info;
  const std::string search_query =
      SearchProviders::ExtractSearchQueryKeywords(url);

  if (!search_query.empty()) {
    auto keyword_segments = GetSegments(search_query);

    if (!keyword_segments.empty()) {
      uint16_t keyword_weight = GetFunnelWeight(search_query);

      signal_info.timestamp_in_seconds = base::Time::Now().ToDoubleT();
      signal_info.segments = keyword_segments;
      signal_info.weight = keyword_weight;
      return signal_info;
    }
  } else {
    SiteInfo info = GetSite(url);

    if (!info.url_netloc.empty()) {
      signal_info.timestamp_in_seconds = base::Time::Now().ToDoubleT();
      signal_info.segments = info.segments;
      signal_info.weight = info.weight;
      return signal_info;
    }
  }

  return signal_info;
}

PurchaseIntentWinningCategoryList
PurchaseIntentClassifier::GetWinningCategories(
    const PurchaseIntentSignalSegmentHistoryMap& history,
    uint16_t max_segments) {
  PurchaseIntentWinningCategoryList winning_categories;
  if (history.empty()) {
    return winning_categories;
  }

  std::multimap<uint16_t, std::string> scores;
  for (const auto& segment_history : history) {
    uint16_t score = GetIntentScoreForHistory(segment_history.second);
    scores.insert(std::make_pair(score, segment_history.first));
  }

  std::multimap<uint16_t, std::string>::reverse_iterator rit;
  for (rit=scores.rbegin(); rit != scores.rend(); ++rit) {
    if (rit->first >= classification_threshold_) {
      winning_categories.push_back(rit->second);
    }

    if (winning_categories.size() >= max_segments) {
      return winning_categories;
    }
  }

  return winning_categories;
}

uint16_t PurchaseIntentClassifier::GetIntentScoreForHistory(
    const PurchaseIntentSignalSegmentHistoryList& history) {
  uint16_t intent_score = 0;

  for (const auto& signal_segment : history) {
    const base::Time signal_decayed_at_in_seconds =
        base::Time::FromDoubleT(signal_segment.timestamp_in_seconds) +
            base::TimeDelta::FromSeconds(signal_decay_time_window_in_seconds_);

    const base::Time now_in_seconds = base::Time::Now();

    if (now_in_seconds > signal_decayed_at_in_seconds) {
      continue;
    }

    intent_score += signal_level_ * signal_segment.weight;
  }

  return intent_score;
}

SiteInfo PurchaseIntentClassifier::GetSite(
    const std::string& url) {
  const GURL visited_url = GURL(url);
  SiteInfo info;

  if (!visited_url.has_host()) {
    return info;
  }

  // TODO(Moritz Haller): Use map since faster than iterating
  for (const auto& site : sites_) {
    const GURL site_url = GURL(site.url_netloc);

    if (!site_url.is_valid()) {
      continue;
    }

    if (SameDomainOrHost(visited_url, site_url,
        net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES)) {
      info = site;
      return info;
    }
  }

  return info;
}

PurchaseIntentSegmentList PurchaseIntentClassifier::GetSegments(
    const std::string& search_query) {
  PurchaseIntentSegmentList segment_list;
  auto search_query_keyword_set = TransformIntoSetOfWords(search_query);

  for (const auto& keyword : segment_keywords_) {
    auto list_keyword_set = TransformIntoSetOfWords(keyword.keywords);

    // Intended behaviour relies on early return from list traversal and
    // implicitely on the ordering of |segment_keywords_| to ensure
    // specific segments are matched over general segments, e.g. "audi a6"
    // segments should be returned over "audi" segments if possible.
    if (IsSubset(search_query_keyword_set, list_keyword_set)) {
      segment_list = keyword.segments;
      return segment_list;
    }
  }

  return segment_list;
}

uint16_t PurchaseIntentClassifier::GetFunnelWeight(
    const std::string& search_query) {
  auto search_query_keyword_set = TransformIntoSetOfWords(search_query);

  uint16_t max_weight = kPurchaseIntentDefaultSignalWeight;
  for (const auto& keyword : funnel_keywords_) {
    auto list_keyword_set = TransformIntoSetOfWords(keyword.keywords);

    if (IsSubset(search_query_keyword_set, list_keyword_set) &&
        keyword.weight > max_weight) {
      max_weight = keyword.weight;
    }
  }

  return max_weight;
}

// TODO(Moritz Haller): Rename to make explicit that method checks set_a is
// subset of set_b but not vice versa
// TODO(https://github.com/brave/brave-browser/issues/8495): Implement Brave
// Ads Purchase Intent keyword matching with std::sets
bool PurchaseIntentClassifier::IsSubset(
    std::vector<std::string> keyword_set_a,
    std::vector<std::string> keyword_set_b) {
  std::sort(keyword_set_a.begin(), keyword_set_a.end());
  std::sort(keyword_set_b.begin(), keyword_set_b.end());

  return std::includes(keyword_set_a.begin(), keyword_set_a.end(),
      keyword_set_b.begin(), keyword_set_b.end());
}

// TODO(https://github.com/brave/brave-browser/issues/8495): Implement Brave
// Ads Purchase Intent keyword matching with std::sets
std::vector<std::string> PurchaseIntentClassifier::TransformIntoSetOfWords(
    const std::string& text) {
  std::string data = text;
  // Remove every character that is not a word/whitespace/underscore character
  RE2::GlobalReplace(&data, "[^\\w\\s]|_", "");
  // Strip subsequent white space characters
  RE2::GlobalReplace(&data, "\\s+", " ");

  std::for_each(data.begin(), data.end(), [](char & c) {
    c = base::ToLowerASCII(c);
  });

  std::stringstream sstream(data);
  std::vector<std::string> set_of_words;
  std::string word;
  uint16_t word_count = 0;

  while (sstream >> word && word_count < kPurchaseIntentWordCountLimit) {
    set_of_words.push_back(word);
    word_count++;
  }

  return set_of_words;
}

bool PurchaseIntentClassifier::FromJson(
    const std::string& json) {
  base::Optional<base::Value> root = base::JSONReader::Read(json);
  if (!root) {
    return false;
  }

  if (base::Optional<int> version = root->FindIntPath("version")) {
    if (kExpectedPurchaseIntentModelVersion != *version) {
      return false;
    }

    version_ = *version;
  }

  // Parsing field: "parameters"
  base::Value* parameters = root->FindKey("parameters");
  if (!parameters->is_dict()) {
    return false;
  }

  if (base::Optional<int> signal_level =
      parameters->FindIntPath("signal_level")) {
    signal_level_ = *signal_level;
  }

  if (base::Optional<int> classification_threshold =
      parameters->FindIntPath("classification_threshold")) {
    classification_threshold_ = *classification_threshold;
  }

  if (base::Optional<int> signal_decay_time_window_in_seconds =
      parameters->FindIntPath("signal_decay_time_window_in_seconds")) {
    signal_decay_time_window_in_seconds_ =
        *signal_decay_time_window_in_seconds;
  }

  // // Parsing field: "segments"
  base::Value* incoming_segemnts = root->FindListPath("segments");
  if (!incoming_segemnts) {
    return false;
  }

  if (!incoming_segemnts->is_list()) {
    return false;
  }

  base::ListValue* list3;
  if (!incoming_segemnts->GetAsList(&list3)) {
    return false;
  }

  std::vector<std::string> segments;
  for (auto& segment : *list3) {
    segments.push_back(segment.GetString());
  }

  // // Parsing field: "segment_keywords"
  base::Value* incoming_segment_keywords =
      root->FindDictPath("segment_keywords");
  if (!incoming_segment_keywords) {
    return false;
  }

  if (!incoming_segment_keywords->is_dict()) {
    return false;
  }

  base::DictionaryValue* dict2;
  if (!incoming_segment_keywords->GetAsDictionary(&dict2)) {
    return false;
  }

  for (base::DictionaryValue::Iterator it(*dict2); !it.IsAtEnd();
      it.Advance()) {
    SegmentKeywordInfo info;
    info.keywords = it.key();
    for (const auto& segment_ix : it.value().GetList()) {
      info.segments.push_back(segments.at(segment_ix.GetInt()));
    }

    segment_keywords_.push_back(info);
  }

  // Parsing field: "funnel_keywords"
  base::Value* incoming_funnel_keywords =
      root->FindDictPath("funnel_keywords");
  if (!incoming_funnel_keywords) {
    return false;
  }

  if (!incoming_funnel_keywords->is_dict()) {
    return false;
  }

  base::DictionaryValue* dict;
  if (!incoming_funnel_keywords->GetAsDictionary(&dict)) {
    return false;
  }

  for (base::DictionaryValue::Iterator it(*dict); !it.IsAtEnd();
      it.Advance()) {
    FunnelKeywordInfo info;
    info.keywords = it.key();
    info.weight = it.value().GetInt();
    funnel_keywords_.push_back(info);
  }

  // // Parsing field: "funnel_sites"
  base::Value* incoming_funnel_sites = root->FindListPath("funnel_sites");
  if (!incoming_funnel_sites) {
    return false;
  }

  if (!incoming_funnel_sites->is_list()) {
    return false;
  }

  base::ListValue* list1;
  if (!incoming_funnel_sites->GetAsList(&list1)) {
    return false;
  }

  // For each set of sites and segments
  for (auto& set : *list1) {
    if (!set.is_dict()) {
      return false;
    }

    // Get all segments...
    base::ListValue* seg_list;
    base::Value* seg_value = set.FindListPath("segments");
    if (!seg_value->GetAsList(&seg_list)) {
      return false;
    }

    std::vector<std::string> site_segments;
    for (auto& seg : *seg_list) {
      site_segments.push_back(segments.at(seg.GetInt()));
    }

    // ...and for each site create info with appended segments
    base::ListValue* site_list;
    base::Value* site_value = set.FindListPath("sites");
    if (!site_value->GetAsList(&site_list)) {
      return false;
    }

    for (const auto& site : *site_list) {
      SiteInfo info;
      info.segments = site_segments;
      info.url_netloc = site.GetString();
      info.weight = 1;
      sites_.push_back(info);
    }
  }

  return true;
}

}  // namespace ads
