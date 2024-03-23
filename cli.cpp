#include <algorithm>
#include <sstream>
#include <cctype>
#include <fstream>

#include "cli.hpp"
#include "primitives/utility.hpp"
#include "tree.hpp"
#include "tt.hpp"
#include "nnue/evaluate.hpp"
#include "scout.hpp"

#include "selfplay.hpp"
#include "pack.hpp"

#include "perft.hpp"

namespace {

template<bool root>
void print_tree(std::vector<size_t> &nodes, size_t parent, int depth) {
    if (!root) {
        nodes.push_back(parent);
        std::cout << g_tree.nodes[parent] << '\n';
    }
    if (!depth)
        return;

    size_t child = root ? 0 : g_tree.first_child(parent);
    while (child != Tree::npos) {
        print_tree<false>(nodes, child, depth - 1);
        child = g_tree.next_child(child);
    }
}

void tree_walker() {
 if (!g_tree.size())
        return;

    int depth = 1;
    std::string token;
    size_t parent = Tree::npos;
    std::vector<size_t> nodes;
    std::ostringstream ss;

    while (true) {
        nodes.clear();
        if (parent == Tree::npos)
            print_tree<true>(nodes, parent, depth);
        else
            print_tree<false>(nodes, parent, depth);

        std::cout << "walker> ";
        std::cin >> token;

        if (token == "quit")
            break;
        else if (token == "setd")
            std::cin >> depth;
        else if (token == "d")
            std::cout << depth << '\n';
        else if (token == "sel") {
            std::cin >> token;
            for (size_t i: nodes) {
                ss.str("");
                ss.clear();
                ss << g_tree.nodes[i].played;
                if (ss.str() == token) {
                    parent = i;
                    break;
                }
            }

        } else if (token == "root") {
            parent = Tree::npos;
        } else if (token == "up") {
            parent = g_tree.parent(parent);
        }
    }
}

} //namespace

UCIContext::UCIContext()
    : board_(&si_)
{
    cfg_.multipv = defopts::MULTIPV;
}

void UCIContext::enter_loop() {
    board_ = Board::start_pos(&si_);
    nnue::refresh_accumulator(board_, si_.acc, WHITE);
    nnue::refresh_accumulator(board_, si_.acc, BLACK);
    st_.reset();

    std::string s, cmd;
    std::istringstream is;

    do {
        if (!std::getline(std::cin, s))
            s = "quit";

        is.str(s);
        is.clear();
        cmd.clear();
        is >> cmd;

        if (cmd == "isready") sync_cout() << "readyok\n";
        else if (cmd == "uci") print_info();
        else if (cmd == "position") parse_position(is);
        else if (cmd == "go") parse_go(is);
        else if (cmd == "setoption") parse_setopt(is);
        else if (cmd == "stop") search_.stop();
        else if (cmd == "ponderhit") search_.stop_pondering();
        else if (cmd == "d") sync_cout() << board_;
        else if (cmd == "tree") tree_walker();
        else if (cmd == "quit") break;

    } while (s != "quit");
}

void UCIContext::parse_position(std::istream &is) {
    std::string s, fen;
    is >> s;
    st_.reset();

    if (s == "fen") {
        while (is >> s && s != "moves")
            fen += s + ' ';
        bool result = board_.load_fen(fen);
        assert(result);
    } else if (s == "startpos") {
        board_ = Board::start_pos(&si_);
        is >> s; //consume "moves"
    } else {
        return;
    }

    if (s != "moves")
        return;

    while (is >> s) {
        Move m = move_from_str(board_, s);
        if (m == MOVE_NONE)
            break;
        st_.push(board_.key(), m);
        board_ = board_.do_move(m, &si_);
    }
    st_.set_start(st_.total_height());
    si_.reset();
}

void UCIContext::parse_go(std::istream &is) {
    std::string token;

    SearchLimits limits;
    bool ponder = false;
    limits.start = timer::now();


    while (is >> token) {
        if (token == "wtime") is >> limits.time[WHITE];
        else if (token == "btime") is >> limits.time[BLACK];
        else if (token == "winc") is >> limits.inc[WHITE];
        else if (token == "binc") is >> limits.inc[BLACK];
        else if (token == "movetime") is >> limits.move_time;
        else if (token == "infinite") limits.infinite = true;
        else if (token == "ponder") ponder = true;
        else if (token == "depth") is >> limits.max_depth;
        else if (token == "nodes") is >> limits.max_nodes;
        else if (token == "perft") {
            parse_go_perft(is);
            return;
        }
    }

    if (!limits.time[WHITE] && !limits.time[BLACK]
            && !limits.move_time)
        limits.infinite = true;

    search_.go(board_, limits, cfg_,
            st_.total_height() ? &st_ : nullptr, ponder);
}

void UCIContext::parse_go_perft(std::istream &is) {
    int depth = 1;
    if ((is >> depth) && depth < 1)
        return;

    auto start = timer::now();
    uint64_t nodes = perft(board_, depth);
    auto delta = timer::now() - start;

    // (nodes / (delta_ms / 1000) / 1'000'000
    uint64_t mnps = nodes / (delta * 1'000);

    sync_cout() << nodes << " nodes @ " << mnps << " mn/s\n";
}

void UCIContext::parse_setopt(std::istream &is) {
    std::string name, t;
    is >> t >> name;

    if (t != "name") return;

    std::transform(name.begin(), name.end(), name.begin(),
        [](char ch) { return std::tolower(ch); });

    auto inrange = [](int x, int a, int b) { return x >= a && x <= b; };

    namespace d = defopts;

    if (name == "hash") {
        if (is >> t; t != "value") return;

        int value = -1;
        if (is >> value && inrange(value, d::TT_SIZE_MIN, d::TT_SIZE_MAX)) {
            search_.stop();
            search_.wait_for_completion();
            g_tt.resize(value);
        }
    } else if (name == "clear") {
        if (is >> t; t != "hash") return;

        g_tt.clear();
    } else if (name == "multipv") {
        if (is >> t; t != "value") return;

        int value = -1;
        if (is >> value && inrange(value, d::MULTIPV_MIN, d::MULTIPV_MAX))
            cfg_.multipv = value;
    } else if (name == "evalfile") {
        if (is >> t; t != "value") return;
        if (!std::getline(is, t)) return;

        const char* path = t.c_str();
        while (*path && std::isspace(*path))
            ++path;

        if (nnue::load_parameters(path)) {
            printf("NNUE initialized from file %s\n", path);
            nnue::refresh_accumulator(board_, si_.acc, WHITE);
            nnue::refresh_accumulator(board_, si_.acc, BLACK);
        } else {
            printf("Failed to initialize NNUE from file %s\n", path);
        }
    } else if (name == "aspdelta") {
        if (is >> t; t != "value") return;

        int value = -1;
        if (is >> value && inrange(value, d::ASP_INIT_MIN, d::ASP_INIT_MAX))
            cfg_.asp_init_delta = value;
    } else if (name == "aspmindepth") {
        if (is >> t; t != "value") return;

        int value = -1;
        if (is >> value && inrange(value, d::ASP_MIN_DEPTH_MIN, d::ASP_MIN_DEPTH_MAX))
            cfg_.asp_min_depth = value;
    } else if (name == "lmrcoeff") {
        if (is >> t; t != "value")
            return;
        if (float value; is >> value)
            init_reduction_tables(value);
    }
}

void UCIContext::print_info() {
    sync_cout() 
        << "id name saturn 1.1\n" 
        << "id author egormoroz\n";

    char buf[1024];

    snprintf(buf, sizeof(buf), 
            "option name Hash type spin default %d min %d max %d\n"
            "option name Ponder type check default false\n"
            "option name clear hash type button\n"
            "option name multipv type spin default %d min %d max %d\n"
            "option name aspdelta type spin default %d min %d max %d\n"
            "option name aspmindepth type spin default %d min %d max %d\n"
            "option name lmrcoeff type string default %.2f\n"
            "option name evalfile type string default %s\n",
            defopts::TT_SIZE, defopts::TT_SIZE_MIN, defopts::TT_SIZE_MAX,
            defopts::MULTIPV, defopts::MULTIPV_MIN, defopts::MULTIPV_MAX,
            defopts::ASP_INIT_DELTA, defopts::ASP_INIT_MIN, defopts::ASP_INIT_MAX,
            defopts::ASP_MIN_DEPTH, defopts::ASP_MIN_DEPTH_MIN, defopts::ASP_MIN_DEPTH_MAX,
            defopts::LMR_COEFF, defopts::NNUE_PATH
    );

    sync_cout() << buf;
    sync_cout() << "uciok\n";
}

int enter_cli(int argc, char **argv) {
    if (argc < 2) {
        (void)(argc);
        (void)(argv);

        UCIContext uci;
        uci.enter_loop();

        return 0;
    }

    if (!strcmp(argv[1], "selfplay")) {
        if (argc != 9) {
            printf("usage: selfplay <out_name> <num_pos> <min_depth> "
                   "<move_time> <n_psv> <max_ld_moves> <n_threads>\n");
            return 1;
        }
        
        const char *out_name = argv[2];
        int num_pos = atol(argv[3]);
        int min_depth = atol(argv[4]);
        int move_time = atol(argv[5]);
        int n_pv = atol(argv[6]);
        int max_ld_moves = atol(argv[7]);
        int n_threads = atol(argv[8]);

        selfplay(out_name, min_depth, move_time, num_pos, 
                n_pv, max_ld_moves, n_threads);

        return 0;
    } else if (!strcmp(argv[1], "packval")) {
        if (argc != 3) {
            printf("usage: packval <pack_fin>\n");
            return 1;
        }

        uint64_t hash = 0;
        bool is_valid = validate_packed_games2(argv[2], hash);
        if (is_valid)
            printf("valid! hash %llu\n", (unsigned long long)hash);
        else
            printf("invalid :-(\n");

        return 0;
    } else if (!strcmp(argv[1], "packstats")) {
        if (argc != 3) {
            printf("usage: packstats <pack_fin>\n");
            return 1;
        }

        std::ifstream fin(argv[2], std::ios::binary);
        if (!fin) {
            printf("could not open file %s\n", argv[2]);
            return 1;
        }

        ChunkHead head;
        uint8_t buffer[head.SIZE];

        unsigned long long cum_hash = 0, n_chains = 0, n_pos = 0;

        while (true) {
            if (!fin.read((char*)buffer, sizeof(buffer)))
                break;

            head.from_bytes(buffer);
            cum_hash ^= head.hash;
            n_chains += head.n_chains;
            n_pos += head.n_pos;

            fin.ignore(PACK_CHUNK_SIZE - head.SIZE);
        }

        printf("Hash %llu\nNumber of chains %llu\nNumber of positions %llu\n",
                cum_hash, n_chains, n_pos);

        return 0;
    } else if (!strcmp(argv[1], "packmerge")) {
        if (argc < 5) {
            printf("usage: packmerge <fout_bin> <n_files> <fbin1> <fbin2>...\n");
            return 1;
        }

        merge_packed_games2((const char**)&argv[4], atol(argv[3]), argv[2]);
        return 0;
    }

    printf("invalid command line arguments\n");
    return 1;
}

