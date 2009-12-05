#include "apply_models.h"

#include <vector>
#include <algorithm>
#include <tr1/unordered_map>
#include <tr1/unordered_set>

#include <boost/functional/hash.hpp>

#include "hg.h"
#include "ff.h"

using namespace std;
using namespace std::tr1;

struct Candidate;
typedef SmallVector JVector;
typedef vector<Candidate*> CandidateHeap;
typedef vector<Candidate*> CandidateList;

// life cycle: candidates are created, placed on the heap
// and retrieved by their estimated cost, when they're
// retrieved, they're incorporated into the +LM hypergraph
// where they also know the head node index they are
// attached to.  After they are added to the +LM hypergraph
// vit_prob_ and est_prob_ fields may be updated as better
// derivations are found (this happens since the successor's
// of derivation d may have a better score- they are
// explored lazily).  However, the updates don't happen
// when a candidate is in the heap so maintaining the heap
// property is not an issue.
struct Candidate {
  int node_index_;                     // -1 until incorporated
                                       // into the +LM forest
  const Hypergraph::Edge* in_edge_;    // in -LM forest
  Hypergraph::Edge out_edge_;
  string state_;
  const JVector j_;
  prob_t vit_prob_;            // these are fixed until the cand
                               // is popped, then they may be updated
  prob_t est_prob_;

  Candidate(const Hypergraph::Edge& e,
            const JVector& j,
            const Hypergraph& out_hg,
            const vector<CandidateList>& D,
            const SentenceMetadata& smeta,
            const ModelSet& models,
            bool is_goal) :
      node_index_(-1),
      in_edge_(&e),
      j_(j) {
    InitializeCandidate(out_hg, smeta, D, models, is_goal);
  }

  // used to query uniqueness
  Candidate(const Hypergraph::Edge& e,
            const JVector& j) : in_edge_(&e), j_(j) {}

  bool IsIncorporatedIntoHypergraph() const {
    return node_index_ >= 0;
  }

  void InitializeCandidate(const Hypergraph& out_hg,
                           const SentenceMetadata& smeta,
                           const vector<vector<Candidate*> >& D,
                           const ModelSet& models,
                           const bool is_goal) {
    const Hypergraph::Edge& in_edge = *in_edge_;
    out_edge_.rule_ = in_edge.rule_;
    out_edge_.feature_values_ = in_edge.feature_values_;
    out_edge_.i_ = in_edge.i_;
    out_edge_.j_ = in_edge.j_;
    out_edge_.prev_i_ = in_edge.prev_i_;
    out_edge_.prev_j_ = in_edge.prev_j_;
    Hypergraph::TailNodeVector& tail = out_edge_.tail_nodes_;
    tail.resize(j_.size());
    prob_t p = prob_t::One();
    // cerr << "\nEstimating application of " << in_edge.rule_->AsString() << endl;
    for (int i = 0; i < tail.size(); ++i) {
      const Candidate& ant = *D[in_edge.tail_nodes_[i]][j_[i]];
      assert(ant.IsIncorporatedIntoHypergraph());
      tail[i] = ant.node_index_;
      p *= ant.vit_prob_;
    }
    prob_t edge_estimate = prob_t::One();
    if (is_goal) {
      assert(tail.size() == 1);
      const string& ant_state = out_hg.nodes_[tail.front()].state_;
      models.AddFinalFeatures(ant_state, &out_edge_);
    } else {
      models.AddFeaturesToEdge(smeta, out_hg, &out_edge_, &state_, &edge_estimate);
    }
    vit_prob_ = out_edge_.edge_prob_ * p;
    est_prob_ = vit_prob_ * edge_estimate;
  }
};

ostream& operator<<(ostream& os, const Candidate& cand) {
  os << "CAND[";
  if (!cand.IsIncorporatedIntoHypergraph()) { os << "PENDING "; }
  else { os << "+LM_node=" << cand.node_index_; }
  os << " edge=" << cand.in_edge_->id_;
  os << " j=<";
  for (int i = 0; i < cand.j_.size(); ++i)
    os << (i==0 ? "" : " ") << cand.j_[i];
  os << "> vit=" << log(cand.vit_prob_);
  os << " est=" << log(cand.est_prob_);
  return os << ']';
}

struct HeapCandCompare {
  bool operator()(const Candidate* l, const Candidate* r) const {
    return l->est_prob_ < r->est_prob_;
  }
};

struct EstProbSorter {
  bool operator()(const Candidate* l, const Candidate* r) const {
    return l->est_prob_ > r->est_prob_;
  }
};

// the same candidate <edge, j> can be added multiple times if
// j is multidimensional (if you're going NW in Manhattan, you
// can first go north, then west, or you can go west then north)
// this is a hash function on the relevant variables from
// Candidate to enforce this.
struct CandidateUniquenessHash {
  size_t operator()(const Candidate* c) const {
    size_t x = 5381;
    x = ((x << 5) + x) ^ c->in_edge_->id_;
    for (int i = 0; i < c->j_.size(); ++i)
      x = ((x << 5) + x) ^ c->j_[i];
    return x;
  }
};

struct CandidateUniquenessEquals {
  bool operator()(const Candidate* a, const Candidate* b) const {
    return (a->in_edge_ == b->in_edge_) && (a->j_ == b->j_);
  }
};

typedef unordered_set<const Candidate*, CandidateUniquenessHash, CandidateUniquenessEquals> UniqueCandidateSet;
typedef unordered_map<string, Candidate*, boost::hash<string> > State2Node;

class CubePruningRescorer {

public:
  CubePruningRescorer(const ModelSet& m,
                      const SentenceMetadata& sm,
                      const Hypergraph& i,
                      int pop_limit,
                      Hypergraph* o) :
      models(m),
      smeta(sm),
      in(i),
      out(*o),
      D(in.nodes_.size()),
      pop_limit_(pop_limit) {
    cerr << "  Rescoring forest (cube pruning, pop_limit = " << pop_limit_ << ')' << endl;
  }

  void Apply() {
    int num_nodes = in.nodes_.size();
    int goal_id = num_nodes - 1;
    int pregoal = goal_id - 1;
    int every = 1;
    if (num_nodes > 100) every = 10;
    assert(in.nodes_[pregoal].out_edges_.size() == 1);
    cerr << "    ";
    for (int i = 0; i < in.nodes_.size(); ++i) {
      if (i % every == 0) cerr << '.';
      KBest(i, i == goal_id);
    }
    cerr << endl;
    cerr << "  Best path: " << log(D[goal_id].front()->vit_prob_)
         << "\t" << log(D[goal_id].front()->est_prob_) << endl;
    out.PruneUnreachable(D[goal_id].front()->node_index_);
    FreeAll();
  }

 private:
  void FreeAll() {
    for (int i = 0; i < D.size(); ++i) {
      CandidateList& D_i = D[i];
      for (int j = 0; j < D_i.size(); ++j)
        delete D_i[j];
    }
    D.clear();
  }

  void IncorporateIntoPlusLMForest(Candidate* item, State2Node* s2n, CandidateList* freelist) {
    Hypergraph::Edge* new_edge = out.AddEdge(item->out_edge_.rule_, item->out_edge_.tail_nodes_);
    new_edge->feature_values_ = item->out_edge_.feature_values_;
    new_edge->edge_prob_ = item->out_edge_.edge_prob_;
    new_edge->i_ = item->out_edge_.i_;
    new_edge->j_ = item->out_edge_.j_;
    new_edge->prev_i_ = item->out_edge_.prev_i_;
    new_edge->prev_j_ = item->out_edge_.prev_j_;
    Candidate*& o_item = (*s2n)[item->state_];
    if (!o_item) o_item = item;
    
    int& node_id = o_item->node_index_;
    if (node_id < 0) {
      Hypergraph::Node* new_node = out.AddNode(in.nodes_[item->in_edge_->head_node_].cat_, item->state_);
      node_id = new_node->id_;
    }
    Hypergraph::Node* node = &out.nodes_[node_id];
    out.ConnectEdgeToHeadNode(new_edge, node);

    // update candidate if we have a better derivation
    // note: the difference between the vit score and the estimated
    // score is the same for all items with a common residual DP
    // state
    if (item->vit_prob_ > o_item->vit_prob_) {
      assert(o_item->state_ == item->state_);    // sanity check!
      o_item->est_prob_ = item->est_prob_;
      o_item->vit_prob_ = item->vit_prob_;
    }
    if (item != o_item) freelist->push_back(item);
  }

  void KBest(const int vert_index, const bool is_goal) {
    // cerr << "KBest(" << vert_index << ")\n";
    CandidateList& D_v = D[vert_index];
    assert(D_v.empty());
    const Hypergraph::Node& v = in.nodes_[vert_index];
    // cerr << "  has " << v.in_edges_.size() << " in-coming edges\n";
    const vector<int>& in_edges = v.in_edges_;
    CandidateHeap cand;
    CandidateList freelist;
    cand.reserve(in_edges.size());
    UniqueCandidateSet unique_cands;
    for (int i = 0; i < in_edges.size(); ++i) {
      const Hypergraph::Edge& edge = in.edges_[in_edges[i]];
      const JVector j(edge.tail_nodes_.size(), 0);
      cand.push_back(new Candidate(edge, j, out, D, smeta, models, is_goal));
      assert(unique_cands.insert(cand.back()).second);  // these should all be unique!
    }
//    cerr << "  making heap of " << cand.size() << " candidates\n";
    make_heap(cand.begin(), cand.end(), HeapCandCompare());
    State2Node state2node;   // "buf" in Figure 2
    int pops = 0;
    while(!cand.empty() && pops < pop_limit_) {
      pop_heap(cand.begin(), cand.end(), HeapCandCompare());
      Candidate* item = cand.back();
      cand.pop_back();
      // cerr << "POPPED: " << *item << endl;
      PushSucc(*item, is_goal, &cand, &unique_cands);
      IncorporateIntoPlusLMForest(item, &state2node, &freelist);
      ++pops;
    }
    D_v.resize(state2node.size());
    int c = 0;
    for (State2Node::iterator i = state2node.begin(); i != state2node.end(); ++i)
      D_v[c++] = i->second;
    sort(D_v.begin(), D_v.end(), EstProbSorter());
    // cerr << "  expanded to " << D_v.size() << " nodes\n";

    for (int i = 0; i < cand.size(); ++i)
      delete cand[i];
    // freelist is necessary since even after an item merged, it still stays in
    // the unique set so it can't be deleted til now
    for (int i = 0; i < freelist.size(); ++i)
      delete freelist[i];
  }

  void PushSucc(const Candidate& item, const bool is_goal, CandidateHeap* pcand, UniqueCandidateSet* cs) {
    CandidateHeap& cand = *pcand;
    for (int i = 0; i < item.j_.size(); ++i) {
      JVector j = item.j_;
      ++j[i];
      if (j[i] < D[item.in_edge_->tail_nodes_[i]].size()) {
        Candidate query_unique(*item.in_edge_, j);
        if (cs->count(&query_unique) == 0) {
          Candidate* new_cand = new Candidate(*item.in_edge_, j, out, D, smeta, models, is_goal);
          cand.push_back(new_cand);
          push_heap(cand.begin(), cand.end(), HeapCandCompare());
          assert(cs->insert(new_cand).second);  // insert into uniqueness set, sanity check
        }
      }
    }
  }

  const ModelSet& models;
  const SentenceMetadata& smeta;
  const Hypergraph& in;
  Hypergraph& out;

  vector<CandidateList> D;   // maps nodes in in-HG to the
                                   // equivalent nodes (many due to state
                                   // splits) in the out-HG.
  const int pop_limit_;
};

struct NoPruningRescorer {
  NoPruningRescorer(const ModelSet& m, const Hypergraph& i, Hypergraph* o) :
      models(m),
      in(i),
      out(*o) {
    cerr << "  Rescoring forest (full intersection)\n";
  }

  void RescoreNode(const int node_num, const bool is_goal) {
  }

  void Apply() {
    int num_nodes = in.nodes_.size();
    int goal_id = num_nodes - 1;
    int pregoal = goal_id - 1;
    int every = 1;
    if (num_nodes > 100) every = 10;
    assert(in.nodes_[pregoal].out_edges_.size() == 1);
    cerr << "    ";
    for (int i = 0; i < in.nodes_.size(); ++i) {
      if (i % every == 0) cerr << '.';
      RescoreNode(i, i == goal_id);
    }
    cerr << endl;
  }

 private:
  const ModelSet& models;
  const Hypergraph& in;
  Hypergraph& out;
};

// each node in the graph has one of these, it keeps track of
void ApplyModelSet(const Hypergraph& in,
                   const SentenceMetadata& smeta,
                   const ModelSet& models,
                   const PruningConfiguration& config,
                   Hypergraph* out) {
  int pl = config.pop_limit;
  if (pl > 100 && in.nodes_.size() > 80000) {
    cerr << "  Note: reducing pop_limit to " << pl << " for very large forest\n";
    pl = 30;
  }
  CubePruningRescorer ma(models, smeta, in, pl, out);
  ma.Apply();
}
