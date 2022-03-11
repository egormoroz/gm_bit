#include "board.hpp"
#include <ostream>
#include "../zobrist.hpp"
#include "../movgen/attack.hpp"
#include "../primitives/utility.hpp"
#include "../core/eval.hpp"

/*
 * FILE: board.cpp
 * All the piece management stuff goes here, as well 
 * as some methods not worth creating a separate file
 * */

void Board::update_pin_info() {
    //could be cheaper, because we look up sliders 3(!) times
    Color us = side_to_move_, them = ~us;
    Square our_king = king_square(us),
           their_king = king_square(them);

    blockers_for_king_[us] = slider_blockers<false>(pieces(them),
            our_king, pinners_[them]);
    blockers_for_king_[them] = slider_blockers<false>(pieces(us),
            their_king, pinners_[us]);

    checkers_ = attackers_to(them, our_king, combined_);
}

Bitboard Board::attackers_to(Color c, Square s, Bitboard blockers) const {
    return (pawn_attacks_bb(~c, s) & pieces(c, PAWN))
        | (attacks_bb<KNIGHT>(s) & pieces(c, KNIGHT))
        | (attacks_bb<BISHOP>(s, blockers) & pieces(c, BISHOP, QUEEN))
        | (attacks_bb<ROOK>(s, blockers) & pieces(c, ROOK, QUEEN))
        | (attacks_bb<KING>(s) & pieces(c, KING));
}

Bitboard Board::attackers_to(Square s, Bitboard blockers) const {
    return (pawn_attacks_bb(BLACK, s) & pieces(WHITE, PAWN))
        | (pawn_attacks_bb(WHITE, s) & pieces(BLACK, PAWN))
        | (attacks_bb<KNIGHT>(s) & pieces(KNIGHT))
        | (attacks_bb<BISHOP>(s, blockers) & pieces(BISHOP, QUEEN))
        | (attacks_bb<ROOK>(s, blockers) & pieces(ROOK, QUEEN))
        | (attacks_bb<KING>(s) & pieces(KING));
}

template Bitboard Board::slider_blockers<true>(Bitboard sliders, 
        Square s, Bitboard &pinners, Bitboard *checkers) const;
template Bitboard Board::slider_blockers<false>(Bitboard sliders, 
        Square s, Bitboard &pinners, Bitboard *checkers) const;

template<bool Checkers>
Bitboard Board::slider_blockers(Bitboard sliders, Square s,
        Bitboard &pinners, Bitboard *checkers) const
{
    assert((combined_ & sliders) == sliders);
    Bitboard blockers = 0;
    pinners = 0;

    Bitboard snipers = ((attacks_bb<BISHOP>(s) & pieces(BISHOP, QUEEN))
        | (attacks_bb<ROOK>(s) & pieces(ROOK, QUEEN))) & sliders;

    while (snipers) {
        Square sniper_sq = pop_lsb(snipers);
        Bitboard b = between_bb(s, sniper_sq) & combined_;
        if (popcnt(b) == 1) {
            blockers |= b;
            pinners |= square_bb(sniper_sq);
        } else if (Checkers && !b) {
            *checkers |= square_bb(sniper_sq);
        }
    }

    return blockers;
}

void Board::put_piece(Piece p, Square s) {
    assert(is_ok(p) && is_ok(s));
    Bitboard sbb = square_bb(s);
    assert(!(combined_ & sbb));

    PieceType pt = type_of(p);
    Color c = color_of(p);

    combined_ |= sbb;
    color_combined_[c] |= sbb;
    pieces_[pt] |= sbb;
    pieces_on_[s] = p;
    material_[c] += mg_value[pt];

    key_ ^= ZOBRIST.psq[p][s];
}

void Board::remove_piece(Square s) {
    assert(is_ok(s));
    Bitboard sbb = square_bb(s);
    assert(combined_ & sbb);
    Piece p = pieces_on_[s];
    PieceType pt = type_of(p);
    Color c = color_of(p);

    combined_ ^= sbb;
    color_combined_[c] ^= sbb;
    pieces_[pt] ^= sbb;
    pieces_on_[s] = NO_PIECE;
    material_[c] -= mg_value[pt];

    key_ ^= ZOBRIST.psq[p][s];
}


Bitboard Board::pieces() const { return combined_; }
Bitboard Board::pieces(Color c) const { return color_combined_[c]; }

Bitboard Board::pieces(PieceType pt) const { return pieces_[pt]; }
Bitboard Board::pieces(PieceType pt1, PieceType pt2) const
{ return pieces_[pt1] | pieces_[pt2]; }

Bitboard Board::pieces(Color c, PieceType pt) const
{ return color_combined_[c] & pieces_[pt]; }

Bitboard Board::pieces(Color c, PieceType pt1, PieceType pt2) const
{ return color_combined_[c] & (pieces_[pt1] | pieces_[pt2]); }

Piece Board::piece_on(Square s) const { return pieces_on_[s]; }

Bitboard Board::checkers() const { return checkers_; }
Bitboard Board::blockers_for_king(Color c) const { return blockers_for_king_[c]; }
Bitboard Board::pinners(Color c) const { return pinners_[c]; }

Square Board::king_square(Color c) const { return lsb(pieces(c, KING)); }

Color Board::side_to_move() const { return side_to_move_; }
Square Board::en_passant() const { return en_passant_; }
CastlingRights Board::castling() const { return castling_; }

uint64_t Board::key() const { return key_; }
int Board::fifty_rule() const { return fifty_; }

int Board::material(Color c) const { return material_[c]; }

namespace {
    constexpr char PIECE_CHAR[PIECE_NB] = {
        ' ', 'P', 'N', 'B', 'R', 'Q', 'K', '?',
        '?', 'p', 'n', 'b', 'r', 'q', 'k'
    };
}

std::ostream& operator<<(std::ostream& os, const Board &b) {
    os << "+---+---+---+---+---+---+---+---+\n";
    for (int r = RANK_8; r >= RANK_1; --r) {
        os << "| ";
        for (int f = FILE_A; f <= FILE_H; ++f) {
            Piece p = b.piece_on(make_square(File(f), Rank(r)));
            os << PIECE_CHAR[p] << " | ";
        }
        os << char('1' + r) << '\n';
        os << "+---+---+---+---+---+---+---+---+\n";
    }

    os << "  a   b   c   d   e   f   g   h\n";
    os << "En passant: ";
    if (b.en_passant() == SQ_NONE) os << '-';
    else os << b.en_passant();

    os << "\nSide to move: " << b.side_to_move();
    os << "\nCastling rights: " << b.castling();
    os << "\nStatic evaluation: " << eval(b) << "\n";

    auto flags = os.flags();
    os << "Key: " << std::hex << b.key() << "\n";
    os.flags(flags);

    os << "Checkers: ";
    Bitboard bb = b.checkers();
    while (bb) {
        Square s = pop_lsb(bb);
        os << s << ' ';
    }
    os << '\n';

    return os;
}

