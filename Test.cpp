/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>
#include <fstream>
#include <string>
#include <vector>

#include <flashlight/flashlight.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "common/Defines.h"
#include "common/Dictionary.h"
#include "common/Transforms.h"
#include "common/Utils.h"
#include "criterion/criterion.h"
#include "module/module.h"
#include "runtime/Data.h"
#include "runtime/Logger.h"
#include "runtime/Serial.h"

using namespace w2l;

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  std::string exec(argv[0]);
  std::vector<std::string> argvs;
  for (int i = 0; i < argc; i++) {
    argvs.emplace_back(argv[i]);
  }
  gflags::SetUsageMessage(
      "Usage: \n " + exec + " [data_path] [dataset_name] [flags]");
  if (argc <= 1) {
    LOG(FATAL) << gflags::ProgramUsage();
  }

  /* ===================== Parse Options ===================== */
  LOG(INFO) << "Parsing command line flags";
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  auto flagsfile = FLAGS_flagsfile;
  if (!flagsfile.empty()) {
    LOG(INFO) << "Reading flags from file " << flagsfile;
    gflags::ReadFromFlagsFile(flagsfile, argv[0], true);
  }

  /* ===================== Create Network ===================== */
  std::shared_ptr<fl::Module> network;
  std::shared_ptr<SequenceCriterion> criterion;
  std::unordered_map<std::string, std::string> cfg;
  LOG(INFO) << "[Network] Reading acoustic model from " << FLAGS_am;
  W2lSerializer::load(FLAGS_am, cfg, network, criterion);
  network->eval();
  criterion->eval();

  LOG(INFO) << "[Network] " << network->prettyString();
  LOG(INFO) << "[Criterion] " << criterion->prettyString();
  LOG(INFO) << "[Network] Number of params: " << numTotalParams(network);

  auto flags = cfg.find(kGflags);
  if (flags == cfg.end()) {
    LOG(FATAL) << "[Network] Invalid config loaded from " << FLAGS_am;
  }
  LOG(INFO) << "[Network] Updating flags from config file: " << FLAGS_am;
  gflags::ReadFlagsFromString(flags->second, gflags::GetArgv0(), true);

  // override with user-specified flags
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  if (!flagsfile.empty()) {
    gflags::ReadFromFlagsFile(flagsfile, argv[0], true);
  }

  LOG(INFO) << "Gflags after parsing \n" << serializeGflags("; ");

  /* ===================== Create Dictionary ===================== */

  auto tokenDict = createTokenDict(pathsConcat(FLAGS_tokensdir, FLAGS_tokens));
  int numClasses = tokenDict.indexSize();
  LOG(INFO) << "Number of classes (network): " << numClasses;

  Dictionary wordDict;
  LexiconMap lexicon;
  if (!FLAGS_lexicon.empty()) {
    lexicon = loadWords(FLAGS_lexicon, FLAGS_maxword);
    wordDict = createWordDict(lexicon);
    LOG(INFO) << "Number of words: " << wordDict.indexSize();
  }

  DictionaryMap dicts = {{kTargetIdx, tokenDict}, {kWordIdx, wordDict}};

  /* ===================== Create Dataset ===================== */
  // Load dataset
  int worldRank = 0;
  int worldSize = 1;
  auto ds = createDataset(FLAGS_test, dicts, lexicon, 1, worldRank, worldSize);

  ds->shuffle(3);
  int nSamples = ds->size();
  if (FLAGS_maxload > 0) {
    nSamples = std::min(nSamples, FLAGS_maxload);
  }
  LOG(INFO) << "[Dataset] Dataset loaded.";

  /* ===================== Test ===================== */
  TestMeters meters;

  EmissionSet emissionSet;
  meters.timer.resume();
  int cnt = 1;
  for (auto& sample : *ds) {
    auto rawEmission = network->forward({fl::input(sample[kInputIdx])}).front();
    auto emission = afToVector<float>(rawEmission);
    auto tokenTarget = afToVector<int>(sample[kTargetIdx]);
    auto wordTarget = afToVector<int>(sample[kWordIdx]);
    auto sampleId = afToVector<std::string>(sample[kSampleIdx]).front();

    auto letterTarget = tkn2Ltr(tokenTarget, tokenDict);
    std::vector<std::string> wordTargetStr;
    if (!FLAGS_lexicon.empty() && FLAGS_criterion != kSeq2SeqCriterion) {
      wordTargetStr = wrdTensor2Words(wordTarget, wordDict);
    } else {
      wordTargetStr = tknTensor2Words(letterTarget, tokenDict);
    }

    // Tokens
    auto tokenPrediction =
        afToVector<int>(criterion->viterbiPath(rawEmission.array()));
    auto letterPrediction = tkn2Ltr(tokenPrediction, tokenDict);

    meters.lerSlice.add(letterPrediction, letterTarget);

    // Words
    std::vector<std::string> wrdPredictionStr =
        tknTensor2Words(letterPrediction, tokenDict);
    meters.werSlice.add(wordTargetStr, wrdPredictionStr);

    if (FLAGS_show) {
      meters.ler.reset();
      meters.wer.reset();
      meters.ler.add(letterPrediction, letterTarget);
      meters.wer.add(wordTargetStr, wrdPredictionStr);

      std::cout << "|T|: " << tensor2String(letterTarget, tokenDict)
                << std::endl;
      std::cout << "|P|: " << tensor2String(letterPrediction, tokenDict)
                << std::endl;
      std::cout << "[sample: " << sampleId << ", WER: " << meters.wer.value()[0]
                << "\%, LER: " << meters.ler.value()[0]
                << "\%, total WER: " << meters.werSlice.value()[0]
                << "\%, total LER: " << meters.lerSlice.value()[0]
                << "\%, progress: " << static_cast<float>(cnt) / nSamples * 100
                << "\%]" << std::endl;
      ++cnt;
      if (cnt == FLAGS_maxload) {
        break;
      }
    }

    /* Save emission and targets */
    int N = rawEmission.dims(0);
    int T = rawEmission.dims(1);
    emissionSet.emissions.emplace_back(emission);
    emissionSet.tokenTargets.emplace_back(tokenTarget);
    emissionSet.wordTargets.emplace_back(wordTargetStr);

    // while testing we use batchsize 1 and hence ds only has 1 sampleid
    emissionSet.sampleIds.emplace_back(
        afToVector<std::string>(sample[kSampleIdx]).front());

    emissionSet.emissionT.emplace_back(T);
    emissionSet.emissionN = N;
  }
  if (FLAGS_criterion == kAsgCriterion) {
    emissionSet.transition = afToVector<float>(criterion->param(0).array());
  }
  emissionSet.gflags = serializeGflags();

  meters.timer.stop();
  std::cout << "---\n[total WER: " << meters.werSlice.value()[0]
            << "\%, total LER: " << meters.lerSlice.value()[0]
            << "\%, time: " << meters.timer.value() << "s]" << std::endl;

  /* ====== Serialize emission and targets for decoding ====== */
  std::string cleanedTestPath = cleanFilepath(FLAGS_test);
  std::string savePath =
      pathsConcat(FLAGS_emission_dir, cleanedTestPath + ".bin");
  LOG(INFO) << "[Serialization] Saving into file: " << savePath;
  W2lSerializer::save(savePath, emissionSet);

  return 0;
}
