#include "../search/search.h"

#include "../search/searchnode.h"
#include "../search/distributiontable.h"

//------------------------
#include "../core/using.h"
//------------------------



void Search::addLeafValue(
  SearchNode& node,
  double winLossValue,
  double noResultValue,
  double scoreMean,
  double scoreMeanSq,
  double lead,
  double weight,
  bool isTerminal,
  bool assumeNoExistingWeight
) {
  double utility =
    getResultUtility(winLossValue, noResultValue)
    + getScoreUtility(scoreMean, scoreMeanSq);

  if(searchParams.subtreeValueBiasFactor != 0 && !isTerminal && node.subtreeValueBiasTableHandle.entry != nullptr) {
    //This is the amount of the direct evaluation of this node that we are going to bias towards the table entry
    const double biasFactor = searchParams.subtreeValueBiasFactor;
    utility += biasFactor * node.subtreeValueBiasTableHandle.getValue();
  }

  utility += getPatternBonus(node.patternBonusHash,getOpp(node.nextPla));

  double utilitySq = utility * utility;
  double weightSq = weight * weight;

  if(assumeNoExistingWeight) {
    while(node.statsLock.test_and_set(std::memory_order_acquire));
    node.stats.winLossValueAvg.store(winLossValue,std::memory_order_release);
    node.stats.noResultValueAvg.store(noResultValue,std::memory_order_release);
    node.stats.scoreMeanAvg.store(scoreMean,std::memory_order_release);
    node.stats.scoreMeanSqAvg.store(scoreMeanSq,std::memory_order_release);
    node.stats.leadAvg.store(lead,std::memory_order_release);
    node.stats.utilityAvg.store(utility,std::memory_order_release);
    node.stats.utilitySqAvg.store(utilitySq,std::memory_order_release);
    node.stats.weightSqSum.store(weightSq,std::memory_order_release);
    node.stats.weightSum.store(weight,std::memory_order_release);
    int64_t oldVisits = node.stats.visits.fetch_add(1,std::memory_order_release);
    node.statsLock.clear(std::memory_order_release);
    // This should only be possible in the extremely rare case that we transpose to a terminal node from a non-terminal node probably due to
    // a hash collision, or that we have a graph history interaction that somehow changes whether a particular path ends the game or not, despite
    // our simpleRepetitionBoundGt logic... such that the node managed to get visits as a terminal node despite not having an nn eval. There's
    // nothing reasonable to do here once we have such a bad collision, so just at least don't crash.
    if(oldVisits != 0) {
      logger->write("WARNING: assumeNoExistingWeight for leaf but leaf already has visits");
    }
  }
  else {
    while(node.statsLock.test_and_set(std::memory_order_acquire));
    double oldWeightSum = node.stats.weightSum.load(std::memory_order_relaxed);
    double newWeightSum = oldWeightSum + weight;

    node.stats.winLossValueAvg.store((node.stats.winLossValueAvg.load(std::memory_order_relaxed) * oldWeightSum + winLossValue * weight)/newWeightSum,std::memory_order_release);
    node.stats.noResultValueAvg.store((node.stats.noResultValueAvg.load(std::memory_order_relaxed) * oldWeightSum + noResultValue * weight)/newWeightSum,std::memory_order_release);
    node.stats.scoreMeanAvg.store((node.stats.scoreMeanAvg.load(std::memory_order_relaxed) * oldWeightSum + scoreMean * weight)/newWeightSum,std::memory_order_release);
    node.stats.scoreMeanSqAvg.store((node.stats.scoreMeanSqAvg.load(std::memory_order_relaxed) * oldWeightSum + scoreMeanSq * weight)/newWeightSum,std::memory_order_release);
    node.stats.leadAvg.store((node.stats.leadAvg.load(std::memory_order_relaxed) * oldWeightSum + lead * weight)/newWeightSum,std::memory_order_release);
    node.stats.utilityAvg.store((node.stats.utilityAvg.load(std::memory_order_relaxed) * oldWeightSum + utility * weight)/newWeightSum,std::memory_order_release);
    node.stats.utilitySqAvg.store((node.stats.utilitySqAvg.load(std::memory_order_relaxed) * oldWeightSum + utilitySq * weight)/newWeightSum,std::memory_order_release);
    node.stats.weightSqSum.store(node.stats.weightSqSum.load(std::memory_order_relaxed) + weightSq,std::memory_order_release);
    node.stats.weightSum.store(newWeightSum,std::memory_order_release);
    node.stats.visits.fetch_add(1,std::memory_order_release);
    node.statsLock.clear(std::memory_order_release);
  }
}

void Search::addCurrentNNOutputAsLeafValue(SearchNode& node, bool assumeNoExistingWeight) {
  const NNOutput* nnOutput = node.getNNOutput();
  assert(nnOutput != NULL);
  //Values in the search are from the perspective of white positive always
  double winProb = (double)nnOutput->whiteWinProb;
  double lossProb = (double)nnOutput->whiteLossProb;
  double noResultProb = (double)nnOutput->whiteNoResultProb;
  double scoreMean = (double)nnOutput->whiteScoreMean;
  double scoreMeanSq = (double)nnOutput->whiteScoreMeanSq;
  double lead = (double)nnOutput->whiteLead;
  double weight = computeWeightFromNNOutput(nnOutput);
  addLeafValue(node,winProb-lossProb,noResultProb,scoreMean,scoreMeanSq,lead,weight,false,assumeNoExistingWeight);
}

double Search::computeWeightFromNNOutput(const NNOutput* nnOutput) const {
  if(!searchParams.useUncertainty)
    return 1.0;
  if(!nnEvaluator->supportsShorttermError())
    return 1.0;

  double scoreMean = (double)nnOutput->whiteScoreMean;
  double utilityUncertaintyWL = searchParams.winLossUtilityFactor * nnOutput->shorttermWinlossError;
  double utilityUncertaintyScore = getApproxScoreUtilityDerivative(scoreMean) * nnOutput->shorttermScoreError;
  double utilityUncertainty = utilityUncertaintyWL + utilityUncertaintyScore;

  double poweredUncertainty;
  if(searchParams.uncertaintyExponent == 1.0)
    poweredUncertainty = utilityUncertainty;
  else if(searchParams.uncertaintyExponent == 0.5)
    poweredUncertainty = sqrt(utilityUncertainty);
  else
    poweredUncertainty = pow(utilityUncertainty, searchParams.uncertaintyExponent);

  double baselineUncertainty = searchParams.uncertaintyCoeff / searchParams.uncertaintyMaxWeight;
  double weight = searchParams.uncertaintyCoeff / (poweredUncertainty + baselineUncertainty);
  return weight;
}


void Search::updateStatsAfterPlayout(SearchNode& node, SearchThread& thread, bool isRoot) {
  //The thread that grabs a 0 from this peforms the recomputation of stats.
  int32_t oldDirtyCounter = node.dirtyCounter.fetch_add(1,std::memory_order_acq_rel);
  assert(oldDirtyCounter >= 0);
  //If we atomically grab a nonzero, then we know another thread must already be doing the work, so we can skip the update ourselves.
  if(oldDirtyCounter > 0)
    return;
  int32_t numVisitsCompleted = 1;
  while(true) {
    //Perform update
    recomputeNodeStats(node,thread,numVisitsCompleted,isRoot);
    //Now attempt to undo the counter
    oldDirtyCounter = node.dirtyCounter.fetch_add(-numVisitsCompleted,std::memory_order_acq_rel);
    int32_t newDirtyCounter = oldDirtyCounter - numVisitsCompleted;
    //If no other threads incremented it in the meantime, so our decrement hits zero, we're done.
    if(newDirtyCounter <= 0) {
      assert(newDirtyCounter == 0);
      break;
    }
    //Otherwise, more threads incremented this more in the meantime. So we need to loop again and add their visits, recomputing again.
    numVisitsCompleted = newDirtyCounter;
    continue;
  }
}

//Recompute all the stats of this node based on its children, except its visits and virtual losses, which are not child-dependent and
//are updated in the manner specified.
//Assumes this node has an nnOutput
void Search::recomputeNodeStats(SearchNode& node, SearchThread& thread, int numVisitsToAdd, bool isRoot) {
  //Find all children and compute weighting of the children based on their values
  vector<MoreNodeStats>& statsBuf = thread.statsBuf;
  int numGoodChildren = 0;

  int childrenCapacity;
  const SearchChildPointer* children = node.getChildren(childrenCapacity);
  double origTotalChildWeight = 0.0;
  for(int i = 0; i<childrenCapacity; i++) {
    const SearchNode* child = children[i].getIfAllocated();
    if(child == NULL)
      break;
    MoreNodeStats& stats = statsBuf[numGoodChildren];

    Loc moveLoc = children[i].getMoveLocRelaxed();
    int64_t edgeVisits = children[i].getEdgeVisits();
    stats.stats = NodeStats(child->stats);

    if(stats.stats.visits <= 0 || stats.stats.weightSum <= 0.0 || edgeVisits <= 0)
      continue;

    double childUtility = stats.stats.utilityAvg;
    stats.selfUtility = node.nextPla == P_WHITE ? childUtility : -childUtility;
    stats.weightAdjusted = stats.stats.getChildWeight(edgeVisits);
    stats.prevMoveLoc = moveLoc;

    origTotalChildWeight += stats.weightAdjusted;
    numGoodChildren++;
  }

  //Always tracks the sum of statsBuf[i].weightAdjusted across the children.
  double currentTotalChildWeight = origTotalChildWeight;

  if(searchParams.useNoisePruning && numGoodChildren > 0 && !(searchParams.antiMirror && mirroringPla != C_EMPTY)) {
    double policyProbsBuf[NNPos::MAX_NN_POLICY_SIZE];
    {
      const NNOutput* nnOutput = node.getNNOutput();
      assert(nnOutput != NULL);
      const float* policyProbs = nnOutput->getPolicyProbsMaybeNoised();
      for(int i = 0; i<numGoodChildren; i++)
        policyProbsBuf[i] = std::max(1e-30, (double)policyProbs[getPos(statsBuf[i].prevMoveLoc)]);
    }
    currentTotalChildWeight = pruneNoiseWeight(statsBuf, numGoodChildren, currentTotalChildWeight, policyProbsBuf);
  }

  {
    double amountToSubtract = 0.0;
    double amountToPrune = 0.0;
    if(isRoot && searchParams.rootNoiseEnabled && !searchParams.useNoisePruning) {
      double maxChildWeight = 0.0;
      for(int i = 0; i<numGoodChildren; i++) {
        if(statsBuf[i].weightAdjusted > maxChildWeight)
          maxChildWeight = statsBuf[i].weightAdjusted;
      }
      amountToSubtract = std::min(searchParams.chosenMoveSubtract, maxChildWeight/64.0);
      amountToPrune = std::min(searchParams.chosenMovePrune, maxChildWeight/64.0);
    }

    downweightBadChildrenAndNormalizeWeight(
      numGoodChildren, currentTotalChildWeight, currentTotalChildWeight,
      amountToSubtract, amountToPrune, statsBuf
    );
  }

  double winLossValueSum = 0.0;
  double noResultValueSum = 0.0;
  double scoreMeanSum = 0.0;
  double scoreMeanSqSum = 0.0;
  double leadSum = 0.0;
  double utilitySum = 0.0;
  double utilitySqSum = 0.0;
  double weightSqSum = 0.0;
  double weightSum = currentTotalChildWeight;
  for(int i = 0; i<numGoodChildren; i++) {
    const NodeStats& stats = statsBuf[i].stats;

    double desiredWeight = statsBuf[i].weightAdjusted;
    double weightScaling = desiredWeight / stats.weightSum;

    winLossValueSum += desiredWeight * stats.winLossValueAvg;
    noResultValueSum += desiredWeight * stats.noResultValueAvg;
    scoreMeanSum += desiredWeight * stats.scoreMeanAvg;
    scoreMeanSqSum += desiredWeight * stats.scoreMeanSqAvg;
    leadSum += desiredWeight * stats.leadAvg;
    utilitySum += desiredWeight * stats.utilityAvg;
    utilitySqSum += desiredWeight * stats.utilitySqAvg;
    weightSqSum += weightScaling * weightScaling * stats.weightSqSum;
  }

  //Also add in the direct evaluation of this node.
  {
    const NNOutput* nnOutput = node.getNNOutput();
    assert(nnOutput != NULL);
    double winProb = (double)nnOutput->whiteWinProb;
    double lossProb = (double)nnOutput->whiteLossProb;
    double noResultProb = (double)nnOutput->whiteNoResultProb;
    double scoreMean = (double)nnOutput->whiteScoreMean;
    double scoreMeanSq = (double)nnOutput->whiteScoreMeanSq;
    double lead = (double)nnOutput->whiteLead;
    double utility =
      getResultUtility(winProb-lossProb, noResultProb)
      + getScoreUtility(scoreMean, scoreMeanSq);

    if(searchParams.subtreeValueBiasFactor != 0 && node.subtreeValueBiasTableHandle.entry != nullptr) {
      if(currentTotalChildWeight > 1e-10) {
        double utilityChildren = utilitySum / currentTotalChildWeight;
        double subtreeValueBiasWeight = pow(origTotalChildWeight, searchParams.subtreeValueBiasWeightExponent);
        double subtreeValueBiasDeltaSum = (utilityChildren - utility) * subtreeValueBiasWeight;

        const double biasFactor = searchParams.subtreeValueBiasFactor;
        utility += biasFactor * node.subtreeValueBiasTableHandle.updateValue(subtreeValueBiasDeltaSum, subtreeValueBiasWeight);
      }
      else {
        const double biasFactor = searchParams.subtreeValueBiasFactor;
        utility += biasFactor * node.subtreeValueBiasTableHandle.getValue();
      }
    }

    double weight = computeWeightFromNNOutput(nnOutput);
    winLossValueSum += (winProb - lossProb) * weight;
    noResultValueSum += noResultProb * weight;
    scoreMeanSum += scoreMean * weight;
    scoreMeanSqSum += scoreMeanSq * weight;
    leadSum += lead * weight;
    utilitySum += utility * weight;
    utilitySqSum += utility * utility * weight;
    weightSqSum += weight * weight;
    weightSum += weight;
  }

  double winLossValueAvg = winLossValueSum / weightSum;
  double noResultValueAvg = noResultValueSum / weightSum;
  double scoreMeanAvg = scoreMeanSum / weightSum;
  double scoreMeanSqAvg = scoreMeanSqSum / weightSum;
  double leadAvg = leadSum / weightSum;
  double utilityAvg = utilitySum / weightSum;
  double utilitySqAvg = utilitySqSum / weightSum;

  double oldUtilityAvg = utilityAvg;
  utilityAvg += getPatternBonus(node.patternBonusHash,getOpp(node.nextPla));
  utilitySqAvg = utilitySqAvg + (utilityAvg * utilityAvg - oldUtilityAvg * oldUtilityAvg);

  //TODO statslock may be unnecessary now with the dirtyCounter mechanism?
  while(node.statsLock.test_and_set(std::memory_order_acquire));
  node.stats.winLossValueAvg.store(winLossValueAvg,std::memory_order_release);
  node.stats.noResultValueAvg.store(noResultValueAvg,std::memory_order_release);
  node.stats.scoreMeanAvg.store(scoreMeanAvg,std::memory_order_release);
  node.stats.scoreMeanSqAvg.store(scoreMeanSqAvg,std::memory_order_release);
  node.stats.leadAvg.store(leadAvg,std::memory_order_release);
  node.stats.utilityAvg.store(utilityAvg,std::memory_order_release);
  node.stats.utilitySqAvg.store(utilitySqAvg,std::memory_order_release);
  node.stats.weightSqSum.store(weightSqSum,std::memory_order_release);
  node.stats.weightSum.store(weightSum,std::memory_order_release);
  node.stats.visits.fetch_add(numVisitsToAdd,std::memory_order_release);
  node.statsLock.clear(std::memory_order_release);
}

void Search::downweightBadChildrenAndNormalizeWeight(
  int numChildren,
  double currentTotalWeight, //The current sum of statsBuf[i].weightAdjusted
  double desiredTotalWeight, //What statsBuf[i].weightAdjusted should sum up to after this function is done.
  double amountToSubtract,
  double amountToPrune,
  vector<MoreNodeStats>& statsBuf
) const {
  if(numChildren <= 0 || currentTotalWeight <= 0.0)
    return;

  if(searchParams.valueWeightExponent == 0 || mirroringPla != C_EMPTY) {
    for(int i = 0; i<numChildren; i++) {
      if(statsBuf[i].weightAdjusted < amountToPrune) {
        currentTotalWeight -= statsBuf[i].weightAdjusted;
        statsBuf[i].weightAdjusted = 0.0;
        continue;
      }
      double newWeight = statsBuf[i].weightAdjusted - amountToSubtract;
      if(newWeight <= 0) {
        currentTotalWeight -= statsBuf[i].weightAdjusted;
        statsBuf[i].weightAdjusted = 0.0;
      }
      else {
        currentTotalWeight -= amountToSubtract;
        statsBuf[i].weightAdjusted = newWeight;
      }
    }

    if(currentTotalWeight != desiredTotalWeight) {
      double factor = desiredTotalWeight / currentTotalWeight;
      for(int i = 0; i<numChildren; i++)
        statsBuf[i].weightAdjusted *= factor;
    }
    return;
  }

  assert(numChildren <= NNPos::MAX_NN_POLICY_SIZE);
  double stdevs[NNPos::MAX_NN_POLICY_SIZE];
  double simpleValueSum = 0.0;
  for(int i = 0; i<numChildren; i++) {
    int64_t numVisits = statsBuf[i].stats.visits;
    assert(numVisits >= 0);
    if(numVisits == 0)
      continue;

    double weight = statsBuf[i].weightAdjusted;
    double precision = 1.5 * sqrt(weight);

    //Ensure some minimum variance for stability regardless of how we change the above formula
    static const double minVariance = 0.00000001;
    stdevs[i] = sqrt(minVariance + 1.0 / precision);
    simpleValueSum += statsBuf[i].selfUtility * weight;
  }

  double simpleValue = simpleValueSum / currentTotalWeight;

  double totalNewUnnormWeight = 0.0;
  for(int i = 0; i<numChildren; i++) {
    if(statsBuf[i].stats.visits == 0)
      continue;

    if(statsBuf[i].weightAdjusted < amountToPrune) {
      currentTotalWeight -= statsBuf[i].weightAdjusted;
      statsBuf[i].weightAdjusted = 0.0;
      continue;
    }
    double newWeight = statsBuf[i].weightAdjusted - amountToSubtract;
    if(newWeight <= 0) {
      currentTotalWeight -= statsBuf[i].weightAdjusted;
      statsBuf[i].weightAdjusted = 0.0;
    }
    else {
      currentTotalWeight -= amountToSubtract;
      statsBuf[i].weightAdjusted = newWeight;
    }

    double z = (statsBuf[i].selfUtility - simpleValue) / stdevs[i];
    //Also just for numeric sanity, make sure everything has some tiny minimum value.
    double p = valueWeightDistribution->getCdf(z) + 0.0001;
    statsBuf[i].weightAdjusted *= pow(p, searchParams.valueWeightExponent);
    totalNewUnnormWeight += statsBuf[i].weightAdjusted;
  }

  //Post-process and normalize to sum to the desired weight
  assert(totalNewUnnormWeight > 0.0);
  double factor = desiredTotalWeight / totalNewUnnormWeight;
  for(int i = 0; i<numChildren; i++)
    statsBuf[i].weightAdjusted *= factor;
}


//Returns the new sum of weightAdjusted
double Search::pruneNoiseWeight(vector<MoreNodeStats>& statsBuf, int numChildren, double totalChildWeight, const double* policyProbsBuf) const {
  if(numChildren <= 1 || totalChildWeight <= 0.00001)
    return totalChildWeight;

  // Children are normally sorted in policy order in KataGo.
  // But this is not guaranteed, because at the root, we might recompute the nnoutput, or when finding the best new child, we have hacks like antiMirror policy
  // and other adjustments. For simplicity, we just consider children in sorted order anyways for this pruning, since it will be close.

  // For any child, if its own utility is lower than the weighted average utility of the children before it, it's downweighted if it exceeds much more than a
  // raw-policy share of the weight.
  double utilitySumSoFar = 0;
  double weightSumSoFar = 0;
  //double rawPolicyUtilitySumSoFar = 0;
  double rawPolicySumSoFar = 0;
  for(int i = 0; i<numChildren; i++) {
    double utility = statsBuf[i].selfUtility;
    double oldWeight = statsBuf[i].weightAdjusted;
    double rawPolicy = policyProbsBuf[i];

    double newWeight = oldWeight;
    if(weightSumSoFar > 0 && rawPolicySumSoFar > 0) {
      double avgUtilitySoFar = utilitySumSoFar / weightSumSoFar;
      double utilityGap = avgUtilitySoFar - utility;
      if(utilityGap > 0) {
        double weightShareFromRawPolicy = weightSumSoFar * rawPolicy / rawPolicySumSoFar;
        //If the child is more than double its proper share of the weight
        double lenientWeightShareFromRawPolicy = 2.0 * weightShareFromRawPolicy;
        if(oldWeight > lenientWeightShareFromRawPolicy) {
          double excessWeight = oldWeight - lenientWeightShareFromRawPolicy;
          double weightToSubtract = excessWeight * (1.0 - exp(-utilityGap / searchParams.noisePruneUtilityScale));
          if(weightToSubtract > searchParams.noisePruningCap)
            weightToSubtract = searchParams.noisePruningCap;

          newWeight = oldWeight - weightToSubtract;
          statsBuf[i].weightAdjusted = newWeight;
        }
      }
    }
    utilitySumSoFar += utility * newWeight;
    weightSumSoFar += newWeight;
    //rawPolicyUtilitySumSoFar += utility * rawPolicy;
    rawPolicySumSoFar += rawPolicy;
  }
  return weightSumSoFar;
}

void Search::updatePolicyBias(SearchNode& node, int childrenCapacity, SearchChildPointer* children) {
  int childPoses[NNPos::MAX_NN_POLICY_SIZE];
  float childProbs[NNPos::MAX_NN_POLICY_SIZE];
  double childWeights[NNPos::MAX_NN_POLICY_SIZE];
  double childSelfUtilities[NNPos::MAX_NN_POLICY_SIZE];

  NNOutput* nnOutput = node.getNNOutput();
  assert(nnOutput != NULL);
  const float* policyProbs = nnOutput->getPolicyProbsMaybeNoised();

  int numChildren = 0;
  for(int i = 0; i<childrenCapacity; i++) {
    const SearchNode* child = children[i].getIfAllocated();
    if(child == NULL)
      break;
    Loc moveLoc = children[i].getMoveLocRelaxed();
    int movePos = getPos(moveLoc);
    float nnPolicyProb = policyProbs[movePos];

    int64_t edgeVisits = children[i].getEdgeVisits();
    double childWeight = child->stats.getChildWeight(edgeVisits);

    if(childWeight < 0.001)
      continue;

    double childUtility = child->stats.utilityAvg.load(std::memory_order_acquire);
    double childSelfUtility = node.nextPla == P_BLACK ? -childUtility : childUtility;

    childPoses[numChildren] = movePos;
    childProbs[numChildren] = nnPolicyProb;
    childWeights[numChildren] = childWeight;
    childSelfUtilities[numChildren] = childSelfUtility;
    numChildren += 1;
  }

  double totalChildWeight = 0.0;
  int highestWeightChildIdx = 0;
  for(int i = 0; i<numChildren; i++) {
    totalChildWeight += childWeights[i];
    if(childWeights[i] > childWeights[highestWeightChildIdx])
      highestWeightChildIdx = i;
  }

  if(totalChildWeight < 0.001)
    return;

  // Take any child whose value is at least as great as the highest weight child
  // and that is at least as surprising.
  // Subtract a small constant to penalize low-explored things and make sure that
  // node really has been explored enough.
  double weightPenalty = 2.0 + childWeights[highestWeightChildIdx] * 0.02;

  int bestChildIdx;
  double bestChildSurprise;
  {
    int i = highestWeightChildIdx;
    double posterior = (childWeights[i] - weightPenalty) / totalChildWeight;
    double surprise = posterior / (childProbs[i] + 0.01);
    bestChildIdx = i;
    bestChildSurprise = surprise;
  }

  for(int i = 0; i<numChildren; i++) {
    if(i != highestWeightChildIdx && childSelfUtilities[i] > childSelfUtilities[highestWeightChildIdx] && childWeights[i] > weightPenalty) {
      double posterior = (childWeights[i] - weightPenalty) / totalChildWeight;
      double surprise = posterior / (childProbs[i] + 0.01);
      if(surprise > bestChildSurprise) {
        bestChildIdx = i;
        bestChildSurprise = surprise;
      }
    }
  }

  int bestChildPos = childPoses[bestChildIdx];
  // if(histLen == rootHistory.moveHistory.size() + 2) {
  //   cout << "TEST " << Location::toString(NNPos::posToLoc(bestChildPos,rootBoard.x_size,rootBoard.y_size,nnXLen,nnYLen),rootBoard) << " " << bestChildSurprise << endl;
  // }

  if(node.policyBiasHandle.entries[bestChildPos] != nullptr) {
    assert(node.policyBiasHandle.entries.size() > bestChildPos);
    // if(histLen == rootHistory.moveHistory.size() + 2 && Location::toString(prevMoveLoc,rootBoard) == "S19") {
    //   cout << "--------------" << endl;
    //   cout
    //     << Location::toString(prevMoveLoc,rootBoard)
    //     << " " << Location::toString(NNPos::posToLoc(bestChildPos,rootBoard.x_size,rootBoard.y_size,nnXLen,nnYLen),rootBoard)
    //     << " " << bestChildSurprise << endl;
    //   for(int i = 0; i<numChildren; i++) {
    //     double posterior = (childWeights[i] - weightPenalty) / totalChildWeight;
    //     double surprise = posterior / (childProbs[i] + 0.01);
    //     cout
    //       << " " << Location::toString(NNPos::posToLoc(childPoses[i],rootBoard.x_size,rootBoard.y_size,nnXLen,nnYLen),rootBoard)
    //       << " " << surprise << " " << childWeights[i] << " " << weightPenalty << " " << totalChildWeight << " " << childProbs[i] << " " << childSelfUtilities[i] << endl;
    //   }
    // }

    double logSurprise = bestChildSurprise <= 1.0 ? 0.0 : std::max(0.0, log(bestChildSurprise));
    node.policyBiasHandle.updateValue(logSurprise * totalChildWeight, totalChildWeight, bestChildPos);
  }
}
