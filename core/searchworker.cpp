#include "searchworker.hpp"
#include "../cli.hpp"
#include "eval.hpp"
#include "../movepicker.hpp"
#include "../primitives/utility.hpp"
#include "../tree.hpp"
#include "../tt.hpp"
#include <algorithm>
#include <sstream>

namespace {

bool can_return_ttscore(const TTEntry &tte, 
    int &alpha, int beta, int depth, int ply)
{
    if (tte.depth8 < depth)
        return false;

    int tt_score = tte.score(ply);
    if (tte.bound8 == BOUND_EXACT) {
        alpha = tt_score;
        return true;
    }
    if (tte.bound8 == BOUND_ALPHA && tt_score <= alpha)
        return true;
    if (tte.bound8 == BOUND_BETA && tt_score >= beta) {
        alpha = beta;
        return true;
    }

    return false;
}

Bound determine_bound(int alpha, int beta, int old_alpha) {
    if (alpha >= beta) return BOUND_BETA;
    if (alpha > old_alpha) return BOUND_EXACT;
    return BOUND_ALPHA;
}

} //namespace


void RootMovePicker::reset(const Board &root){
    Move ttm = MOVE_NONE;
    TTEntry tte;
    if (g_tt.probe(root.key(), tte)) {
        if (!root.is_valid_move(ttm = Move(tte.move16)))
            ttm = MOVE_NONE;
    }

    MovePicker mp(root, ttm);
    cur_ = num_moves_ = 0;
    for (Move m = mp.next<false>(); m != MOVE_NONE; 
            m = mp.next<false>())
    {
        moves_[num_moves_++] = { m, 0, 0, 0 };
    }
}

Move RootMovePicker::next() {
    if (cur_ >= num_moves_)
        return MOVE_NONE;
    return moves_[cur_++].move;
}

void RootMovePicker::update_last(int score, uint64_t nodes) {
    assert(cur_ > 0 && cur_ <= num_moves_);
    auto &last = moves_[cur_ - 1];
    last.nodes = nodes;
    last.prev_score = last.score;
    last.score = score;
}

int RootMovePicker::num_moves() const {
    return num_moves_;
}

void RootMovePicker::complete_iter() {
    std::sort(moves_.begin(), moves_.begin() + num_moves_,
        [](const RootMove &x, const RootMove &y)
    {
        if (x.score != y.score) return x.score > y.score;
        return x.prev_score > y.prev_score;
        /* return x.nodes > y.nodes; */
    });
    cur_ = 0;
}

SearchWorker::SearchWorker() 
    : root_(Board::start_pos())
{
    loop_.start(std::bind(
            &SearchWorker::iterative_deepening, this));
}

void SearchWorker::go(const Board &root, const Stack &st, 
        const SearchLimits &limits)
{
    loop_.pause();
    loop_.wait_for_completion();

    root_ = root;
    stack_ = st;
    limits_ = limits;
    man_.start = limits.start;
    man_.max_time = limits_.move_time;
    stats_.reset();
    rmp_.reset(root_);
    hist_.reset();

    man_.init(limits, root.side_to_move(), st.total_height());

    memset(counters_.data(), 0, sizeof(counters_));
    memset(followups_.data(), 0, sizeof(followups_));

    loop_.resume();
}

void SearchWorker::stop() {
    loop_.pause();
}

void SearchWorker::wait_for_completion() {
    loop_.wait_for_completion();
}

void SearchWorker::check_time() {
    if (stats_.nodes & 2047)
        return;
    if (loop_.keep_going() && !limits_.infinite
            && man_.out_of_time())
        loop_.pause();
}

void SearchWorker::iterative_deepening() {
    Move pv[MAX_DEPTH]{};
    int pv_len = 0, score = 0, prev_score, ebf = 1;
    uint64_t prev_nodes, nodes = 0;
    std::ostringstream ss;

    if (rmp_.num_moves() == 1) {
        sync_cout() << "bestmove " << rmp_.next() << '\n';
        return;
    }

    auto report = [&](int d) {
        auto elapsed = timer::now() - limits_.start;
        uint64_t nps = stats_.nodes * 1000 / (elapsed + 1);

        pv_len = g_tt.extract_pv(root_, pv, d);

        ss.str("");
        ss.clear();
        float fhf = stats_.fail_high_first 
            / float(stats_.fail_high + 1);
        ss << "info score " << Score{score}
           << " depth " << d
           << " nodes " << stats_.nodes
           << " time " << elapsed
           << " nps " << nps
           << " fhf " << fhf
           << " ebf " << ebf
           << " pv ";

        for (int i = 0; i < pv_len; ++i)
            ss << pv[i] << ' ';
        sync_cout() << ss.str() << '\n';
    };

    prev_nodes = 1;
    score = search_root(-VALUE_MATE, VALUE_MATE, 1);
    nodes = stats_.nodes;
    report(1);
    for (int d = 2; d <= limits_.max_depth; ++d) {
        g_tree.clear();
        prev_nodes = nodes;
        uint64_t before = stats_.nodes;
        prev_score = score;
        TimePoint start = timer::now();

        score = aspriration_window(score, d);
        if (!loop_.keep_going())
            break;
        report(d);

        nodes = stats_.nodes - before;
        ebf = static_cast<int>((nodes + prev_nodes - 1) 
                / std::max(1ull, prev_nodes));

        TimePoint now = timer::now(), 
              time_left = man_.start + man_.max_time - now;
        if (abs(score - prev_score) < 8 && !limits_.infinite
                && !limits_.move_time && now - start >= time_left)
            break; //assume we don't have enough time to go 1 ply deeper

        if (abs(score) >= VALUE_MATE - d)
            break;
    }
    sync_cout() << "bestmove " << pv[0] << '\n';
}

int SearchWorker::aspriration_window(int score, int depth) {
    if (depth <= 5)
        return search_root(-VALUE_MATE, VALUE_MATE, depth);

    int delta = 16, alpha = score - delta, 
        beta = score + delta;
    while (loop_.keep_going()) {
        score = search_root(alpha, beta, depth);

        if (score <= alpha) {
            beta = (alpha + beta) / 2;
            alpha = std::max(-VALUE_MATE, alpha - delta);
        } else if (score >= beta) {
            beta = std::min(+VALUE_MATE, beta + delta);
        } else {
            break;
        }

        delta += delta / 2;
    }

    return score;
}

int SearchWorker::search_root(int alpha, int beta, int depth) {
    if (root_.half_moves() >= 100 
        || (!root_.checkers() && root_.is_material_draw())
        || stack_.is_repetition(root_.key(), root_.half_moves()))
        return 0;

    TTEntry tte;
    Move ttm = MOVE_NONE;
    if (g_tt.probe(root_.key(), tte)) {
        if (can_return_ttscore(tte, alpha, beta, depth, 0))
            return alpha;
        if (ttm = Move(tte.move16); !root_.is_valid_move(ttm))
            ttm = MOVE_NONE;
    }

    Move best_move = MOVE_NONE;
    int best_score = -VALUE_MATE, old_alpha = alpha,
        moves_tried = 0;
    Board bb;
    for (Move m = rmp_.next(); m != MOVE_NONE; m = rmp_.next()) {
        uint64_t nodes_before = stats_.nodes;
        size_t ndx = g_tree.begin_node(m, alpha, beta, depth, 0);
        bb = root_.do_move(m);
        stack_.push(root_.key(), m);

        int score;
        if (!moves_tried || depth <= 6) {
            score = -search(bb, -beta, -alpha, depth - 1);
        } else {
            score = -search(bb, -(alpha + 1), -alpha, depth - 1);
            if (score > alpha && score < beta)
                score = -search(bb, -beta, -alpha, depth - 1);
        }

        ++moves_tried;
        stack_.pop();
        g_tree.end_node(ndx, score);
        rmp_.update_last(score, stats_.nodes - nodes_before);

        if (score > best_score) {
            best_score = score;
            best_move = m;
        }

        if (score > alpha)
            alpha = score;
        if (score >= beta) {
            alpha = beta;
            break;
        }
    }
    
    rmp_.complete_iter();
    if (loop_.keep_going()) {
        g_tt.store(TTEntry(root_.key(), alpha, 
            determine_bound(alpha, beta, old_alpha),
            depth, best_move, 0, false));
    }

    return alpha;
}

int SearchWorker::search(const Board &b, int alpha, 
        int beta, int depth) 
{
    const int ply = stack_.height();

    check_time();
    if (!loop_.keep_going())
        return 0;

    //Mate distance pruning
    int mated_score = stack_.mated_score();
    alpha = std::max(alpha, mated_score);
    beta = std::min(beta, -mated_score - 1);
    if (alpha >= beta)
        return alpha;

    if (depth <= 0)
        return b.checkers() ? quiescence<true>(b, alpha, beta)
            : quiescence<false>(b, alpha, beta);
    stats_.nodes++;
    if (stack_.capped())
        return eval(b);

    g_tt.prefetch(b.key());
    if (b.half_moves() >= 100 
        || (!b.checkers() && b.is_material_draw())
        || stack_.is_repetition(b.key(), b.half_moves()))
        return 0;

    TTEntry tte;
    Move ttm = MOVE_NONE;
    if (g_tt.probe(b.key(), tte)) {
        if (ttm = Move(tte.move16); !b.is_valid_move(ttm))
            ttm = MOVE_NONE;
        
        if (can_return_ttscore(tte, alpha, beta, 
                    depth, ply))
        {
            if (ttm && b.is_quiet(ttm))
                hist_.add_bonus(b, ttm, depth * depth);
            return alpha;
        }
    }

    if (!ttm && depth >= 5) {
        search(b, alpha, beta, depth - 2);
        if (g_tt.probe(b.key(), tte) 
                && !b.is_valid_move(ttm = Move(tte.move16)))
            ttm = MOVE_NONE;
    }

    Move opp_move = stack_.at(ply - 1).move,
         prev = MOVE_NONE, followup = MOVE_NONE;
    if (ply >= 2) {
        prev = stack_.at(ply - 2).move;
        followup = followups_[from_to(prev)];
    }
    auto &entry = stack_.at(ply);
    MovePicker mp(b, ttm, entry.killers, &hist_,
            counters_[from_to(opp_move)],
            followup);

    int best_score = -VALUE_MATE, moves_tried = 0,
        old_alpha = alpha;
    Move best_move = MOVE_NONE;
    Board bb;
    for (Move m = mp.next<false>(); m != MOVE_NONE; 
            m = mp.next<false>()) 
    {
        size_t ndx = g_tree.begin_node(m, alpha, beta, 
                depth, ply);
        bb = b.do_move(m);
        stack_.push(b.key(), m);

        int score = -search(bb, -beta, -alpha, depth - 1);

        stack_.pop();
        g_tree.end_node(ndx, score);
        ++moves_tried;

        if (score > best_score) {
            best_score = score;
            best_move = m;
        }

        if (score > alpha)
            alpha = score;
        if (score >= beta)
            break;
    }

    if (!moves_tried) {
        if (b.checkers())
            return stack_.mated_score();
        return 0;
    }

    if (alpha >= beta) {
        alpha = beta;
        stats_.fail_high++;
        stats_.fail_high_first += moves_tried == 1;
        if (b.is_quiet(best_move)) {
            if (entry.killers[0] != best_move) {
                entry.killers[1] = entry.killers[0];
                entry.killers[0] = best_move;
            }

            hist_.add_bonus(b, best_move, depth * depth);
            counters_[from_to(opp_move)] = best_move;

            if (prev)
                followups_[from_to(prev)] = best_move;
        }
    }

    if (loop_.keep_going()) {
        g_tt.store(TTEntry(b.key(), alpha, 
            determine_bound(alpha, beta, old_alpha),
            depth, best_move, ply, false));
    }

    return alpha;
}

/* template int SearchWorker::quiescence<true>( */
/*         const Board &b, int alpha, int beta); */
/* template int SearchWorker::quiescence<false>( */
/*         const Board &b, int alpha, int beta); */

template<bool with_evasions>
int SearchWorker::quiescence(const Board &b, 
        int alpha, int beta) 
{
    check_time();
    if (!loop_.keep_going() || b.half_moves() >= 100
        || b.is_material_draw()
        || stack_.is_repetition(b.key(), b.half_moves()))
        return 0;

    if (stack_.capped())
        return eval(b);

    stats_.nodes++;
    stats_.qnodes++;

    //Mate distance pruning
    int mated_score = stack_.mated_score();
    alpha = std::max(alpha, mated_score);
    beta = std::min(beta, -mated_score - 1);
    if (alpha >= beta)
        return alpha;

    if constexpr (!with_evasions) {
        int stand_pat = eval(b);
        alpha = std::max(alpha, stand_pat);
        if (alpha >= beta)
            return beta;
    }

    MovePicker mp(b);
    Board bb;
    constexpr bool only_tacticals = !with_evasions;
    int moves_tried = 0;
    for (Move m = mp.next<only_tacticals>(); m != MOVE_NONE; 
            m = mp.next<only_tacticals>(), ++moves_tried)
    {
        size_t ndx = g_tree.begin_node(m, alpha, beta, 
                0, stack_.height());
        bb = b.do_move(m);
        stack_.push(b.key(), m);

        //filter out perpetual checks
        bool gen_evasions = !with_evasions && bb.checkers();
        int score = gen_evasions ? -quiescence<true>(bb, -beta, -alpha)
            : -quiescence<false>(bb, -beta, -alpha);

        stack_.pop();
        g_tree.end_node(ndx, score);

        if (score > alpha)
            alpha = score;
        if (score >= beta) 
            return beta;
    }

    if (with_evasions && !moves_tried)
        return stack_.mated_score();

    return alpha;
}

