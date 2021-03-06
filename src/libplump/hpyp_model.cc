/*
 * Copyright 2008-2016 Jan Gasthaus
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "libplump/hpyp_model.h"

#include <cmath>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>

#include "libplump/utils.h"
#include "libplump/subseq.h"
#include "libplump/stirling.h"
#include "libplump/random.h"
#include "libplump/hpyp_restaurants.h"


namespace gatsby { namespace libplump {

HPYPModel::HPYPModel(seq_type& seq,
                     INodeManager& nodeManager,
                     const IAddRemoveRestaurant& restaurant,
                     IParameters& parameters,
                     int numTypes) 
    : seq(seq), 
      contextTree_(new ContextTree(nodeManager, seq)), 
      contextTree(*contextTree_), 
      restaurant(restaurant),
      parameters(parameters), 
      numTypes(numTypes) {
    baseProb = 1./((double) numTypes);
  }
 

void HPYPModel::insertRoot(e_type obs) {
  WrappedNodeList root_path = this->contextTree.findLongestSuffix(0,0);
  d_vec discount_path = this->parameters.getDiscounts(root_path);
  d_vec concentration_path = this->parameters.getConcentrations(root_path,
                                                                discount_path);
  d_vec prob_path = this->computeProbabilityPath(root_path,
                                                 discount_path,
                                                 concentration_path,
                                                 obs);
  this->updatePath(root_path,
                   prob_path,
                   discount_path,
                   concentration_path,
                   obs);
}


/**
 * Insert a context into the tree and handle a potentially occuring 
 * split.
 */
WrappedNodeList HPYPModel::insertContext(l_type start, l_type stop) {
  typedef ContextTree::InsertionResult InsertionResult;

  // insert context
  InsertionResult insertionResult = contextTree.insert(start, stop);

  // handle split if one occurred
  if (insertionResult.action != InsertionResult::INSERT_ACTION_NO_SPLIT) { 
    WrappedNode nodeA, nodeC;
    WrappedNodeList::iterator i = insertionResult.path.end();

    // move iterator to point to the split node which is either the 
    // last or second to last element on the path, depending on 
    // whether the inserted node was a suffix
    switch (insertionResult.action) {
      case InsertionResult::INSERT_ACTION_SPLIT :
        // split where inserted node is not a suffix of a node in 
        // the tree
        i--; i--; // split node is above the newly inserted node
        break;
      case InsertionResult::INSERT_ACTION_SPLIT_SUFFIX :
        // split where inserted node is a suffix -> inserted node
        // is the shorter node
        i--; // split node is the newly inserted node
        break;
      case InsertionResult::INSERT_ACTION_NO_SPLIT :
        break;
    }
    nodeC = *i;
    i--; // move iterator to parent
    nodeA = *i;
    this->handleSplit(nodeA, 
                      insertionResult.splitChild,
                      nodeC); 
  }
  return insertionResult.path;    
}


d_vec HPYPModel::insertContextAndObservation(l_type start, 
                                             l_type stop,
                                             e_type obs) {
  // insert context (and handle a potential split)
  WrappedNodeList path = insertContext(start, stop);

  // insert observation
  d_vec discountPath = this->parameters.getDiscounts(path);
  d_vec concentrationPath = this->parameters.getConcentrations(path,
                                                               discountPath);
  d_vec probabilityPath = this->computeProbabilityPath(path,
                                                       discountPath,
                                                       concentrationPath,
                                                       obs);
  this->parameters.accumulateParameterGradient(this->restaurant, 
      path, probabilityPath, discountPath, concentrationPath, obs);

  this->updatePath(path, probabilityPath, discountPath,
                   concentrationPath, obs);

  // static int j = 0;
  //if (j == 1) {
    this->parameters.stepParameterGradient(10e-4);
  //  j = 0;
  //}
  

  return probabilityPath; 
}


d_vec HPYPModel::insertObservation(l_type start, l_type stop, e_type obs, 
    WrappedNodeList* cached_path) {
  tracer << "HPYPModel::insertObservation(" << start << ", " << stop 
         << ", " << obs << ")" << std::endl;

  WrappedNodeList path;
  if (cached_path != NULL) {
    path = *cached_path;
  } else {
    path = this->contextTree.findLongestSuffix(start,stop);
  }
  
  tracer << "  HPYPModel::insertObservation: longest suffix path: " 
         << std::endl << this->contextTree.pathToString(path) << std::endl;
  
  d_vec discount_path = this->parameters.getDiscounts(path);
  d_vec concentration_path = this->parameters.getConcentrations(path,
                                                                discount_path);
  d_vec prob_path = this->computeProbabilityPath(path,
                                                 discount_path,
                                                 concentration_path,
                                                 obs);
  this->updatePath(path, prob_path, discount_path,
                   concentration_path, obs);
  return prob_path; 
}


void HPYPModel::removeObservation(
    l_type start, l_type stop, e_type obs, 
    const HPYPModel::PayloadDataPath& payloadDataPath,
    WrappedNodeList* cached_path) {
  tracer << "HPYPModel::removeObservation(" << start << ", " << stop 
         << ", " << obs << ")" << std::endl;

  WrappedNodeList path;
  if (cached_path != NULL) {
    path = *cached_path;
  } else {
    path = this->contextTree.findLongestSuffix(start,stop);
  }
  assert(path.back().end == (*cached_path).back().end);
  
  tracer << "  HPYPModel::removeObservation: longest suffix path: " 
         << std::endl << contextTree.pathToString(path) << std::endl;
  
  d_vec discountPath = this->parameters.getDiscounts(path);
  
  this->removeObservationFromPath(path, 
                                  discountPath,
                                  obs,
                                  payloadDataPath);
}


void HPYPModel::removeAddSweep(l_type start, l_type stop) {
  // start timer
  clock_t start_t,end_t;
  start_t = clock();
  for (int i = start; i < stop; ++i) {
    HPYPModel::PayloadDataPath payloadDataPath;
    WrappedNodeList path = this->contextTree.findNode(start, i);
    //WrappedNodeList p2 = this->contextTree.findLongestSuffix(start, i);
    //std::cout << path.back().end << ", " << p2.back().end
    //          << ", " << path.size() << ", " << p2.size() <<std::endl;
    //assert(path.back().end == p2.back().end);
    this->removeObservation(start, i, this->seq[i], payloadDataPath, &path);
    this->insertObservation(start, i, this->seq[i], &path);

    if (i%10000==0) {
      end_t = clock();
      std::cerr << makeProgressBarString(i/(double)stop) << " " 
                << ((double)i*CLOCKS_PER_SEC)/(end_t-start_t) << " chars/sec" 
                <<  "\r";
    }
  }
}


d_vec HPYPModel::computeLosses(l_type start, l_type stop) {
  d_vec losses;

  // deal with first symbol: add loss and insert customer
  losses.push_back(log2((double) this->numTypes));
  insertRoot(this->seq[start]);

  // start timer
  clock_t start_t,end_t;
  start_t = clock();

  for (l_type i=start+1; i < stop; i++) {
    d_vec prob_path = this->insertContextAndObservation(start,i,this->seq[i]);
    double prob = prob_path[prob_path.size()-2];
    losses.push_back(-log2(prob));
    
    if (i%10000==0) {
      end_t = clock();
      std::cerr << makeProgressBarString(i/(double)stop) << " " 
                << ((double)i*CLOCKS_PER_SEC)/(end_t-start_t) << " chars/sec" 
                <<  "\r";
    }

  }
  end_t = clock();

  std::cerr << makeProgressBarString(1) << " " 
            << ((double)stop*CLOCKS_PER_SEC)/(end_t-start_t) << " chars/sec" 
            <<  std::endl;
  
  return losses;
}


d_vec HPYPModel::computeLossesWithDeletion(l_type start, l_type stop, l_type lag) {
  d_vec losses;

  // deal with first symbol: add loss and insert customer
  losses.push_back(log2((double) this->numTypes));
  insertRoot(this->seq[start]);

  // start timer
  clock_t start_t,end_t;
  start_t = clock();

  for (l_type i=start+1; i < stop; i++) {
    d_vec prob_path = this->insertContextAndObservation(start,i,this->seq[i]);
    double prob = prob_path[prob_path.size()-2];
    losses.push_back(-log2(prob));
    if (i - lag >= start) {
      HPYPModel::PayloadDataPath payloadDataPath;
      WrappedNodeList path = this->contextTree.findNode(start, i - lag);
      this->removeObservation(start, i - lag, this->seq[i - lag], payloadDataPath, &path);
    }

    if (i%10000==0) {
      end_t = clock();
      std::cerr << makeProgressBarString(i/(double)stop) << " " 
        << ((double)i*CLOCKS_PER_SEC)/(end_t-start_t) << " chars/sec" 
        <<  "\r";
    }

  }
  end_t = clock();

  std::cerr << makeProgressBarString(1) << " " 
    << ((double)stop*CLOCKS_PER_SEC)/(end_t-start_t) << " chars/sec" 
    <<  std::endl;

  return losses;
}


d_vec HPYPModel::predictSequence(l_type start, l_type stop, PredictMode mode) {
  d_vec probs;
  for (l_type i = start; i < stop; i++) {
    switch(mode) {
      case ABOVE:
        probs.push_back(this->predict(start, i, this->seq[i]));
        break;
      case FRAGMENT:
        probs.push_back(this->predictWithFragmentation(start, i, this->seq[i]));
        break;
      case BELOW:
        probs.push_back(this->predictBelow(start, i, this->seq[i]));
        break;
    }
  }
  return probs;
}


void HPYPModel::buildTree(l_type stop) {
  this->insertRoot(this->seq[0]);
  for (l_type i=1; i < stop; ++i) {
    this->insertContextAndObservation(0, i, this->seq[i]);
  }
}


void HPYPModel::updateTree(l_type start, l_type stop) {
  for (l_type i = start; i < stop; ++i) {
    this->insertContextAndObservation(0, i, this->seq[i]);
  }
}


double HPYPModel::predict(l_type start, l_type stop, e_type obs) {
  WrappedNodeList path = this->contextTree.findLongestSuffix(start,stop);
  d_vec discount_path = this->parameters.getDiscounts(path);
  d_vec concentration_path = this->parameters.getConcentrations(path,
                                                                discount_path);

  d_vec prob_path = this->computeProbabilityPath(path,
                                                 discount_path,
                                                 concentration_path,
                                                 obs);
  return prob_path.back();
}


/**
 * Predict prob; in the case of required fragmentation, predict form 
 * _below_ the split point!
 */
double HPYPModel::predictBelow(l_type start, l_type stop, e_type obs) {
  WrappedNodeList path = 
      this->contextTree.findLongestSuffixVirtual(start,stop).second;
  d_vec discount_path = this->parameters.getDiscounts(path);
  d_vec concentration_path = this->parameters.getConcentrations(path,
                                                                discount_path);

  d_vec prob_path = this->computeProbabilityPath(path,
                                                 discount_path,
                                                 concentration_path,
                                                 obs);
  return prob_path.back();
}


double HPYPModel::predictWithFragmentation(l_type start, 
                                           l_type stop,
                                           e_type obs) {
  std::pair<int, WrappedNodeList> path = contextTree.findLongestSuffixVirtual(
      start, stop);

  d_vec discountPath = parameters.getDiscounts(path.second);
  d_vec concentrationPath = parameters.getConcentrations(path.second,
                                                         discountPath);
  d_vec probabilityPath = this->computeProbabilityPath(path.second,
                                                       discountPath,
                                                       concentrationPath,
                                                       obs);

  double probability = 0;
  if (path.first != 0) {
    // create a new payload for the node we are predicting from
    // fragmentation -- last probability on the path needs to be recomputed
    // by creating a new, split node of length path.second. 
    void* splitNode = this->restaurant.getFactory().make();
    WrappedNodeList::iterator it = path.second.end();
    it--; it--; // one before last; parent of node we need to split
    int parentLength = it->end - it->start; 
    // path.first is length of parent after split
    double discountAfter = parameters.getDiscount(path.first, it->end - it->start);
    it++; // last node
    double discountFragmented = this->parameters.getDiscount(parentLength,
                                                             path.first);
    this->restaurant.updateAfterSplit(
        it->payload,
        splitNode,
        discountPath.back(),
        discountFragmented,
        true); // update splitNode only
    double concentrationFragmented = this->parameters.getConcentration(
        discountFragmented, parentLength, path.first);
    probability = this->restaurant.computeProbability(
        splitNode, obs, probabilityPath[probabilityPath.size()-2],
        discountFragmented, concentrationFragmented);
    this->restaurant.getFactory().recycle(splitNode);
  } else {
    probability = probabilityPath.back();
  }
  return probability;
}


d_vec HPYPModel::predictiveDistribution(l_type start, l_type stop) {
  d_vec predictive;
  predictive.reserve(this->numTypes);
  WrappedNodeList path = this->contextTree.findLongestSuffix(start,stop);
  d_vec discount_path = this->parameters.getDiscounts(path);
  d_vec concentration_path = this->parameters.getConcentrations(path,
                                                                discount_path);

  for(int i = 0; i < this->numTypes; ++i) {
    d_vec prob_path = this->computeProbabilityPath(path,
                                                   discount_path,
                                                   concentration_path,
                                                   i);
    predictive.push_back(prob_path.back());
  }

  return predictive;
}


d_vec HPYPModel::predictiveDistributionWithMixing(l_type start, 
                                                  l_type stop, 
                                                  d_vec& mixingWeights) {
  d_vec predictive;
  predictive.reserve(this->numTypes);
  const WrappedNodeList path = this->contextTree.findLongestSuffix(start, stop);
  const d_vec discount_path = this->parameters.getDiscounts(path);
  const d_vec concentration_path 
      = this->parameters.getConcentrations(path, discount_path);

  for(int i = 0; i < this->numTypes; ++i) {
    d_vec prob_path = this->computeProbabilityPath(path,
                                                   discount_path,
                                                   concentration_path,
                                                   i);
    double prob = 0;
    double sum = 0;
    for (int j = 0; 
         j < std::min<int>(mixingWeights.size(), prob_path.size());
         ++j) {
      prob += mixingWeights[j]*prob_path[j];
      sum += mixingWeights[j];
    }
    predictive.push_back(prob + (1 - sum)*prob_path.back());
  }

  return predictive;
}
        

/**
 * Given a path from the root to some node (not a leaf), 
 * perform add/remove Gibbs sampling of the last node by repeatedly 
 * removing and adding customers, cus times for each type s.
 */
void HPYPModel::addRemoveSamplePath(
    const WrappedNodeList& path, 
    const d_vec& discountPath, 
    const d_vec& concentrationPath, 
    const HPYPModel::PayloadDataPath& payloadDataPath,
    double baseProb) {
  assert(path.size() > 0);
  assert(path.size() == discountPath.size());
  assert(path.size() == concentrationPath.size());

  bool useAdditionalData = payloadDataPath.size() == path.size();
  void* main = path.back().payload;
  const IAddRemoveRestaurant& r = this->restaurant; // shortcut
  
  IHPYPBaseRestaurant::TypeVector types = r.getTypeVector(main);
  for(IHPYPBaseRestaurant::TypeVectorIterator it = types.begin(); 
      it != types.end(); ++it) { // for each type of customer 

    e_type type = *it;
    l_type cw = r.getC(main, type);
    
    if (cw == 1) {
      continue; // no point in reseating in a 1 customer restaurant
    }

    d_vec probabilityPath = this->computeProbabilityPath(path,
                                                         discountPath,
                                                         concentrationPath,
                                                         type);
    WrappedNodeList::const_iterator current;
    for (l_type i = 0; i < cw; ++i) { // for each customer of this type
      current = --path.end(); // set current to last restaurant in path
      
      // index into d/alpha vectors for current restaurant
      int j = discountPath.size() - 1;
      
      bool goUp = true;
      while(goUp && j != -1) {
        void* additionalData = NULL;
        if (useAdditionalData) {
          additionalData = payloadDataPath[j].get();
        }
        bool removed = r.removeCustomer(current->payload,
                                        type,
                                        discountPath[j],
                                        additionalData);
        if (removed) {
          --current;
          --j;
        } else {
          goUp = false;
        }
      }

      // recompute probabilities back down; need not recompute last probability
      while(j < (int)probabilityPath.size() - 1) {
        if (j == -1) {
          // can't recompute base distribution probabilities at prob_path[0]
          j = 0; // 
          ++current;
        }
        probabilityPath[j+1] = r.computeProbability(current->payload, 
                                                    type,
                                                    probabilityPath[j],
                                                    discountPath[j],
                                                    concentrationPath[j]);
        ++j; 
        ++current;
      }

      // set current and j to the last restaurant on path
      current = --path.end(); 
      j = discountPath.size() - 1; 
      goUp = true;
      while(goUp && j != -1) {
        void* additionalData = NULL;
        if (useAdditionalData) {
          additionalData = payloadDataPath[j].get();
        }
        bool inserted = r.addCustomer(current->payload, 
                                      type,
                                      probabilityPath[j],
                                      discountPath[j],
                                      concentrationPath[j], 
                                      additionalData);
        if (inserted) {
          current--;
          j--;
        } else {
          goUp = false;
        }
      }

    }

  }

}


/**
 * Given a path from the root to some node (not a leaf), 
 * perform add/remove Gibbs sampling of the last node by repeatedly 
 * removing and adding customers, cus times for each type s.
 */
void HPYPModel::directGibbsSamplePath(
    const WrappedNodeList& path, 
    const d_vec& discountPath, 
    const d_vec& concentrationPath, 
    const HPYPModel::PayloadDataPath& payloadDataPath,
    double baseProb) {
  assert(path.size() > 0);
  assert(path.size() == discountPath.size());
  assert(path.size() == concentrationPath.size());

  bool useAdditionalData = payloadDataPath.size() == path.size();
  // XXX: HACK! We assume this is the type of addData for the used restaurant
  stirling_generator_full_log *stirlingGenCurrent = NULL;
  stirling_generator_full_log *stirlingGenParent = NULL;
  void* main = path.back().payload;
  const BaseCompactRestaurant& r = (BaseCompactRestaurant&)this->restaurant; // shortcut
  
  IHPYPBaseRestaurant::TypeVector types = r.getTypeVector(main);
  for(IHPYPBaseRestaurant::TypeVectorIterator it = types.begin(); 
      it != types.end(); ++it) { // for each type of customer 

    e_type type = *it;
    l_type cw = r.getC(main, type);
    
    if (cw == 1) {
      continue; // no point in reseating in a 1 customer restaurant
    }

    WrappedNodeList::const_iterator current;
    current = --path.end(); // set current to last restaurant in path

    // index into d/alpha vectors for current restaurant
    int j = discountPath.size() - 1;

    bool goUp = true;
    while(goUp && j != -1) {
      goUp = false;
      stirlingGenCurrent = (stirling_generator_full_log*)payloadDataPath[j].get();
      void* currentPayload = (*current).payload;
      void* parentPayload = NULL; // initialized below
      int currentCw = r.getC(currentPayload, type);
      int currentTw = r.getT(currentPayload, type);
      int otherT = r.getT(currentPayload) - currentTw;
      std::vector<double> logProbs(currentCw, 0);
      std::vector<double> logProbs1(currentCw, 0);
      std::vector<double> logProbs2(currentCw, 0);
      std::vector<double> logProbs3(currentCw, 0);
      std::vector<double> logProbs4(currentCw, 0);
      if (j > 0) { // not at the top, so we have a CRP parent
        stirlingGenParent = (stirling_generator_full_log*)payloadDataPath[j-1].get();
        current--; // move to parent
        parentPayload = (*current).payload;
        current++;
        
        int parentTw = r.getT(parentPayload, type);
        int parentCw = r.getC(parentPayload, type);
        int parentOtherC = r.getC(parentPayload) - currentTw;
        for (int tw = 1; tw <= currentCw; ++tw) {
          int newParentCw = parentCw - currentTw + tw;
          if (newParentCw < parentTw) {
            logProbs4[tw-1] = -INFINITY;
          } else {
            logProbs1[tw-1] = logKramp(concentrationPath[j] + discountPath[j], discountPath[j], otherT + tw - 1);
            logProbs2[tw-1] = - logKramp(concentrationPath[j-1] + 1, 1, parentOtherC + tw - 1);
            logProbs3[tw-1] = stirlingGenCurrent->getLog(currentCw, tw);
            logProbs4[tw-1] = stirlingGenParent->getLog(newParentCw, parentTw);
            //std::cerr <<  logKramp(concentrationPath[j] + discountPath[j], discountPath[j], otherT + tw - 1) << std::endl;
            //std::cerr <<  logKramp(concentrationPath[j-1], 1, parentOtherC + tw - 1)<< std::endl;
            //std::cerr <<  stirlingGenCurrent->getLog(cw, tw)<< std::endl;
            //std::cerr << cw << ", " << tw << std::endl;
            //std::cerr <<  stirlingGenParent->getLog(newParentCw, parentTw)<< std::endl;
            //std::cerr << newParentCw << ", " << parentTw << std::endl;
          }
        }
        //std::cerr << parentCw << ", " << parentTw << ", " << parentOtherC << ", " << currentTw << ", " << otherT << std::endl;
      } else { // at the root, take base prob into account
        for (int tw = 1; tw <= currentCw; ++tw) {
          logProbs1[tw-1] = logKramp(concentrationPath[j] + discountPath[j], discountPath[j], otherT + tw - 1);
          logProbs2[tw-1] = stirlingGenCurrent->getLog(currentCw, tw);
          logProbs3[tw-1] = tw * log(baseProb);
        }
      }
      // subtract max for stability
      //std::cerr << iterableToString(logProbs1) << std::endl;
      //std::cerr << iterableToString(logProbs2) << std::endl;
      //std::cerr << iterableToString(logProbs3) << std::endl;
      //std::cerr << iterableToString(logProbs4) << std::endl;
      subMax_vec(logProbs1);
      subMax_vec(logProbs2);
      subMax_vec(logProbs3);
      subMax_vec(logProbs4);
      add_vec(logProbs, logProbs1);
      add_vec(logProbs, logProbs2);
      add_vec(logProbs, logProbs3);
      add_vec(logProbs, logProbs4);
      subMax_vec(logProbs);


      //std::cerr << iterableToString(logProbs1) << std::endl;
      //std::cerr << iterableToString(logProbs2) << std::endl;
      //std::cerr << iterableToString(logProbs3) << std::endl;
      //std::cerr << iterableToString(logProbs4) << std::endl;
      //std::cerr << iterableToString(logProbs) << std::endl;
      exp_vec(logProbs);
      //std::cerr << iterableToString(logProbs) << std::endl;
      // if (max == -INFINITY) { // if all choices are improbable, choose uniformly
      //     // we can do better by normalizing the component individually
      //   for (int ii = 0; ii < logProbs.size(); ++ii)
      //     logProbs[ii] = 1.0;
      // }
      //std::cerr << iterableToString(logProbs) << std::endl;
      int sampledTw = sample_unnormalized_pdf(logProbs, 0) + 1;
      //std::cerr << "Old tw: " << currentTw << ", newTw: " << sampledTw << std::endl;


      r.setT(currentPayload, type, sampledTw);
      if (j > 0) { 
        int newCw =  r.getC(parentPayload, type) - currentTw + sampledTw;
        assert(newCw >= r.getT(parentPayload, type));
        r.setC(parentPayload, type, newCw);
      }

      if (sampledTw != currentTw) {
        --current;
        --j;
      } else {
        goUp = false;
      }
    }

  }

}


boost::shared_ptr<void> HPYPModel::makeAdditionalDataPtr(void* payload, 
                                                         double discount, 
                                                         double concentration) const {
  return boost::shared_ptr<void>(
      this->restaurant.createAdditionalData(payload,
        discount,
        concentration),
      boost::bind(&IAddRemoveRestaurant::freeAdditionalData, 
        &(this->restaurant),
        _1));
}


void HPYPModel::runGibbsSampler(bool directGibbs) {
  ContextTree::DFSPathIterator pathIterator = contextTree.getDFSPathIterator();
  d_vec discountPath = parameters.getDiscounts(*pathIterator);
  d_vec concentrationPath = parameters.getConcentrations(*pathIterator, 
                                                         discountPath);

  // initialize payloadDataPath; by using shared_ptr with the proper
  // destruction function, all clean-up should be automatic.
  HPYPModel::PayloadDataPath payloadDataPath;
  int j = 0;
  for (WrappedNodeList::const_iterator it = (*pathIterator).begin();
       it != (*pathIterator).end(); ++it) {
      payloadDataPath.push_back(this->makeAdditionalDataPtr(
            it->payload, discountPath[j], concentrationPath[j]));
    j++;
  }

  if (directGibbs) {
    this->directGibbsSamplePath(*pathIterator, discountPath, concentrationPath,
                                payloadDataPath, baseProb);
  } else {
    this->addRemoveSamplePath(*pathIterator, discountPath, concentrationPath,
                              payloadDataPath, baseProb);
  }

  size_t pathLength = (*pathIterator).size();
  
  while(pathIterator.hasMore()) { // loop over all paths in the tree
    ++pathIterator;
    if ((*pathIterator).size() == 0) {
      break;
    }

    if ((*pathIterator).size() == pathLength) {
      // sibling
      discountPath.pop_back();
      parameters.extendDiscounts(*pathIterator, discountPath);
      concentrationPath.pop_back();
      parameters.extendConcentrations(*pathIterator,
                                      discountPath, 
                                      concentrationPath);
      payloadDataPath.pop_back();
      payloadDataPath.push_back(
          this->makeAdditionalDataPtr((*pathIterator).back().payload, 
                                      discountPath.back(), 
                                      concentrationPath.back()));

    } else {
      if ((*pathIterator).size() == pathLength - 1) {
        // we went up -- just drop the last term
        discountPath.pop_back();
        concentrationPath.pop_back();
        payloadDataPath.pop_back();
      } else {
        // we went up one and then down some number of levels -- recompute
        discountPath.pop_back();
        concentrationPath.pop_back();
        parameters.extendDiscounts(*pathIterator, discountPath);
        parameters.extendConcentrations(*pathIterator,
                                        discountPath,
                                        concentrationPath);
        payloadDataPath.pop_back();
        WrappedNodeList::const_iterator it = (*pathIterator).begin();
        // move it to the first item not covered by the payloadDataPath
        for (size_t i = 0; i < payloadDataPath.size(); ++i) {
          ++it;
        }
        for (size_t i = payloadDataPath.size(); i < discountPath.size(); ++i) {
          payloadDataPath.push_back(
              this->makeAdditionalDataPtr(it->payload, 
                                          discountPath[i], 
                                          concentrationPath[i]));

          ++it;
        }
        assert(it == (*pathIterator).end());
      }
    }
    pathLength = (*pathIterator).size();
    
    tracer << (*pathIterator).size() << " " << discountPath.size() << " " 
           << concentrationPath.size() << " " <<  payloadDataPath.size() 
           << std::endl;
    
    
    if (directGibbs) {
      this->directGibbsSamplePath(*pathIterator, discountPath, concentrationPath,
                                  payloadDataPath, baseProb);
    } else {
      this->addRemoveSamplePath(*pathIterator, discountPath, concentrationPath,
                                payloadDataPath, baseProb);
    }
  }
}


d_vec HPYPModel::computeProbabilityPath(const WrappedNodeList& path, 
                                        const d_vec& discount_path, 
                                        const d_vec& concentration_path,
                                        e_type obs) {
  d_vec out;
  out.reserve(path.size() + 1);
  
  double prob = this->baseProb; // base distribution
  out.push_back(prob);
  
  int j = 0;
  for(WrappedNodeList::const_iterator it = path.begin();
      it!= path.end();
      ++it) {
    prob = this->restaurant.computeProbability(it->payload, 
                                               obs,
                                               prob,
                                               discount_path[j],
                                               concentration_path[j]);
    out.push_back(prob);
    j++;
  } 
  return out;
}


void HPYPModel::updatePath(const WrappedNodeList& path, 
                           const d_vec& prob_path, 
                           const d_vec& discount_path, 
                           const d_vec& concentration_path, 
                           e_type obs) {

  unsigned int j=path.size()-1;
  double newTable = 1;
  for(WrappedNodeList::const_reverse_iterator it = path.rbegin(); 
      it != path.rend();
      ++it) {
    newTable = this->restaurant.addCustomer(it->payload,
                                            obs,
                                            prob_path[j],
                                            discount_path[j],
                                            concentration_path[j],
                                            NULL,
                                            newTable);
    if (newTable==0) {
      break;
    }
    j--;
  }
}
    

void HPYPModel::removeObservationFromPath(
    const WrappedNodeList& path,
    const d_vec& discountPath,
    e_type obs,
    const HPYPModel::PayloadDataPath& payloadDataPath) {
  int j = path.size()-1;


  double frac_t = 1;

  for(WrappedNodeList::const_reverse_iterator it = path.rbegin();
    it != path.rend(); it++) {

    void* payloadData = NULL;
    if (payloadDataPath.size() == path.size()) {
      payloadData = payloadDataPath[j].get();
    }

    frac_t = this->restaurant.removeCustomer(it->payload,
                                             obs,
                                             discountPath[j],
                                             payloadData, frac_t);
    if (frac_t == 0.)
      break;
    j--;
  }
}
        

void HPYPModel::handleSplit(const WrappedNode& nodeA,
                            const WrappedNode& nodeB, 
                            const WrappedNode& nodeC) {
    int lengthA = nodeA.end - nodeA.start;
    int lengthB = nodeB.end - nodeB.start;
    int lengthC = nodeC.end - nodeC.start;
    
    // parent context should be shorter than both its children 
    assert(lengthA < lengthB && lengthA < lengthC);
    // length of node that required splitting should be longer than the result
    assert(lengthC < lengthB);

    double discBBeforeSplit = this->parameters.getDiscount(lengthA, lengthB);
    double discBAfterSplit  = this->parameters.getDiscount(lengthC, lengthB);
    this->restaurant.updateAfterSplit(nodeB.payload, 
                                      nodeC.payload, 
                                      discBBeforeSplit,
                                      discBAfterSplit);
}


bool HPYPModel::checkConsistency(const WrappedNode& node, 
                      const std::list<WrappedNode>& children) const {
  bool consistent = this->restaurant.checkConsistency(node.payload);
  std::map<e_type, int> table_counts;
  for(std::list<WrappedNode>::const_iterator it = children.begin();
      it != children.end(); ++it) {
    IHPYPBaseRestaurant::TypeVector keys = 
        this->restaurant.getTypeVector(it->payload);
    for(IHPYPBaseRestaurant::TypeVectorIterator key_it = keys.begin();
        key_it != keys.end(); ++key_it) {
      table_counts[*key_it] += this->restaurant.getT(it->payload, *key_it);
    }
  }

  for(std::map<e_type, int>::iterator it = table_counts.begin();
      it != table_counts.end(); ++it) {
    consistent = (this->restaurant.getC(node.payload, (*it).first) 
                  >= (*it).second) && consistent;
    if (!consistent) {
      std::cerr << "Child table sum is: " << (*it).second 
                << ", parent customers is: " 
                << this->restaurant.getC(node.payload, (*it).first) 
                << std::endl;
    }
  }
  return consistent;
}


void HPYPModel::prunePath(WrappedNodeList& path) {

}


bool HPYPModel::checkConsistency() const {
  CheckConsistencyVisitor v(*this);
  this->contextTree.visitDFSWithChildren(v);
  return v.consistent;
}
 

double HPYPModel::computeLogRestaurantProb(
    const WrappedNodeList& path, 
    const d_vec& discountPath, 
    const d_vec& concentrationPath, 
    const HPYPModel::PayloadDataPath& payloadDataPath,
    double baseProb) const {
  assert(path.size() > 0);
  assert(path.size() == discountPath.size());
  assert(path.size() == concentrationPath.size());

  void* payload = path.back().payload;
  const BaseCompactRestaurant& r = (BaseCompactRestaurant&)this->restaurant; // shortcut
  double logProb = 0;
  int j = discountPath.size() - 1;
  l_type c = r.getC(payload); 
  if (c == 1) {
      // deterministic restaurant
      return 0;
  }
  l_type t = r.getT(payload);
  //std::cerr << "c: " << c << ", t: " << t << std::endl;
  //std::cerr << "a: " << concentrationPath[j] << ", d: " << discountPath[j] << std::endl;
  logProb += logKramp(concentrationPath[j] + discountPath[j], discountPath[j], t - 1);
  //std::cerr << logProb << std::endl;
  logProb -= logKramp(concentrationPath[j] + 1, 1, c - 1);
  //std::cerr << logProb << std::endl;

  
  stirling_generator_full_log *stirlingGen = (stirling_generator_full_log*)payloadDataPath[j].get();
  IHPYPBaseRestaurant::TypeVector types = r.getTypeVector(payload);
  for(IHPYPBaseRestaurant::TypeVectorIterator it = types.begin(); 
      it != types.end(); ++it) { // for each type of customer 

    e_type type = *it;
    l_type cw = r.getC(payload, type);
    l_type tw = r.getT(payload, type);
    
    logProb += stirlingGen->getLog(cw, tw);

    if (j == 0) { // at the root, take base prob into account
      logProb += tw * log(baseProb);
    }
  }
  //std::cerr << logProb << std::endl;
  return logProb;
}

double HPYPModel::computeLogJoint() const {
  double logJoint = 0;
  ContextTree::DFSPathIterator pathIterator = contextTree.getDFSPathIterator();
  d_vec discountPath = parameters.getDiscounts(*pathIterator);
  d_vec concentrationPath = parameters.getConcentrations(*pathIterator, 
                                                         discountPath);

  // initialize payloadDataPath; by using shared_ptr with the proper
  // destruction function, all clean-up should be automatic.
  HPYPModel::PayloadDataPath payloadDataPath;
  int j = 0;
  for (WrappedNodeList::const_iterator it = (*pathIterator).begin();
       it != (*pathIterator).end(); ++it) {
      payloadDataPath.push_back(this->makeAdditionalDataPtr(
            it->payload, discountPath[j], concentrationPath[j]));
    j++;
  }

  logJoint += computeLogRestaurantProb(*pathIterator, discountPath, concentrationPath, payloadDataPath, baseProb);

  size_t pathLength = (*pathIterator).size();
  
  while(pathIterator.hasMore()) { // loop over all paths in the tree
    ++pathIterator;
    if ((*pathIterator).size() == 0) {
      break;
    }

    if ((*pathIterator).size() == pathLength) {
      // sibling
      discountPath.pop_back();
      parameters.extendDiscounts(*pathIterator, discountPath);
      concentrationPath.pop_back();
      parameters.extendConcentrations(*pathIterator,
                                      discountPath, 
                                      concentrationPath);
      payloadDataPath.pop_back();
      payloadDataPath.push_back(
          this->makeAdditionalDataPtr((*pathIterator).back().payload, 
                                      discountPath.back(), 
                                      concentrationPath.back()));

    } else {
      if ((*pathIterator).size() == pathLength - 1) {
        // we went up -- just drop the last term
        discountPath.pop_back();
        concentrationPath.pop_back();
        payloadDataPath.pop_back();
      } else {
        // we went up one and then down some number of levels -- recompute
        discountPath.pop_back();
        concentrationPath.pop_back();
        parameters.extendDiscounts(*pathIterator, discountPath);
        parameters.extendConcentrations(*pathIterator,
                                        discountPath,
                                        concentrationPath);
        payloadDataPath.pop_back();
        WrappedNodeList::const_iterator it = (*pathIterator).begin();
        // move it to the first item not covered by the payloadDataPath
        for (size_t i = 0; i < payloadDataPath.size(); ++i) {
          ++it;
        }
        for (size_t i = payloadDataPath.size(); i < discountPath.size(); ++i) {
          payloadDataPath.push_back(
              this->makeAdditionalDataPtr(it->payload, 
                                          discountPath[i], 
                                          concentrationPath[i]));

          ++it;
        }
        assert(it == (*pathIterator).end());
      }
    }
    pathLength = (*pathIterator).size();

    logJoint += computeLogRestaurantProb(*pathIterator, discountPath, concentrationPath, payloadDataPath, baseProb);
    
  }
  return logJoint;
}


HPYPModel::ToStringVisitor::ToStringVisitor(seq_type& seq, 
    const IHPYPBaseRestaurant& restaurant) 
    : outstream(), seq(seq), restaurant(restaurant) {}


void HPYPModel::ToStringVisitor::operator()(const WrappedNode& n) {
  for (l_type i = 0; i < n.depth; ++i) {
    this->outstream << " "; 
  }
  this->outstream << SubSeq::toString(n.start, n.end, seq);
  this->outstream << " " << this->restaurant.toString(n.payload);
  this->outstream << std::endl;
}


HPYPModel::CheckConsistencyVisitor::CheckConsistencyVisitor(
    const HPYPModel& model) : consistent(true), model(model) {}


void HPYPModel::CheckConsistencyVisitor::operator()(
    WrappedNode& n, std::list<WrappedNode>& children) {
  bool nodeConsistent = this->model.checkConsistency(n, children);
  if (!nodeConsistent) {
    std::cerr << "Node " << n.toString() << " not consistent!" << std::endl;
  }
  consistent = consistent && nodeConsistent;
}


HPYPModel::LogJointVisitor::LogJointVisitor(
    const HPYPModel& model) : logJoint(0), model(model) {}


void HPYPModel::LogJointVisitor::operator()(
    // FIXME
    WrappedNode& n, std::list<WrappedNode>& children) {
    int c = model.restaurant.getC(n.payload);
    int t = model.restaurant.getT(n.payload);
    IHPYPBaseRestaurant::TypeVector keys = 
        model.restaurant.getTypeVector(n.payload);
    for(IHPYPBaseRestaurant::TypeVectorIterator key_it = keys.begin();
        key_it != keys.end(); ++key_it) {
      int cw = model.restaurant.getC(n.payload, *key_it);
      int tw = model.restaurant.getT(n.payload, *key_it);
    }
}

std::string HPYPModel::toString() {
  HPYPModel::ToStringVisitor visitor(this->seq, this->restaurant);
  this->contextTree.visitDFS(visitor);
  return visitor.outstream.str();
}


}} // namespace gatsby::libplump
