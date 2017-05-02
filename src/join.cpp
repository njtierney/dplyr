#include "pch.h"
#include <dplyr/main.h>

#include <tools/utils.h>
#include <tools/SymbolString.h>

#include <dplyr/JoinVisitorImpl.h>

namespace dplyr {

inline bool is_bare_vector(SEXP x) {
  SEXP att = ATTRIB(x);

  // only allow R_Names. as in R's do_isvector
  while (att != R_NilValue) {
    SEXP tag = TAG(att);
    if (!(tag == R_NamesSymbol || tag == Rf_install("comment"))) return false;
    att = CDR(att);
  }

  return true;
}

void warn_bad_var(const SymbolString& var_left, const SymbolString& var_right,
                  std::string message, bool warn = true) {
  if (!warn)
    return;

  if (var_left == var_right) {
    std::string var_utf8 = var_left.get_utf8_cstring();
    Rf_warningcall(R_NilValue,
      "Variable `%s` %s",
      var_utf8.c_str(),
      message.c_str()
    );
  } else {
    std::string left_utf8 = var_left.get_utf8_cstring();
    std::string right_utf8 = var_right.get_utf8_cstring();
    Rf_warningcall(R_NilValue,
      "Variable `%s`/`%s` %s",
      left_utf8.c_str(),
      right_utf8.c_str(),
      message.c_str()
    );
  }

}

void check_attribute_compatibility(SEXP left, SEXP right, const SymbolString& name_left, const SymbolString& name_right) {
  // Ignore attributes for POSIXct
  if (Rf_inherits(left, "POSIXct") && Rf_inherits(right, "POSIXct")) {
    return;
  }

  // Otherwise rely on R function based on all.equal
  static Function attr_equal = Function("attr_equal", Environment::namespace_env("dplyr"));
  bool ok = as<bool>(attr_equal(left, right));
  if (!ok) {
    warn_bad_var(name_left, name_right, "has different attributes on RHS and LHS of join");
  }
}

CharacterVector reencode_factor(IntegerVector x);

R_xlen_t get_first_reencode_pos(const CharacterVector& xc) {
  R_xlen_t len = xc.length();
  for (R_xlen_t i = 0; i < len; ++i) {
    SEXP xci = xc[i];
    if (xci != NA_STRING && !IS_ASCII(xci) && !IS_UTF8(xci)) {
      return i;
    }
  }

  return len;
}

CharacterVector reencode_char(SEXP x) {
  if (Rf_isFactor(x)) return reencode_factor(x);

  CharacterVector xc(x);
  R_xlen_t first = get_first_reencode_pos(xc);
  if (first >= xc.length()) return x;

  CharacterVector ret(Rf_duplicate(xc));

  R_xlen_t len = ret.length();
  for (R_xlen_t i = first; i < len; ++i) {
    SEXP reti = ret[i];
    if (reti != NA_STRING && !IS_ASCII(reti) && !IS_UTF8(reti)) {
      ret[i] = String(Rf_translateCharUTF8(reti), CE_UTF8);
    }
  }

  return ret;
}

CharacterVector reencode_factor(IntegerVector x) {
  CharacterVector levels(reencode_char(get_levels(x)));
  CharacterVector ret(x.length());

  R_xlen_t nlevels = levels.length();

  R_xlen_t len = x.length();
  for (R_xlen_t i = 0; i < len; ++i) {
    int xi = x[i];
    if (xi <= 0 || xi > nlevels)
      ret[i] = NA_STRING;
    else
      ret[i] = levels[xi - 1];
  }

  return ret;
}

template <int LHS_RTYPE, bool ACCEPT_NA_MATCH>
JoinVisitor* date_join_visitor_right(SEXP left, SEXP right, const SymbolString& name_left, const SymbolString& name_right) {
  switch (TYPEOF(right)) {
  case INTSXP:
    return new DateJoinVisitor<LHS_RTYPE, INTSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
  case REALSXP:
    return new DateJoinVisitor<LHS_RTYPE, REALSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
  default:
    stop("Date objects should be represented as integer or numeric");
  }
}

template <bool ACCEPT_NA_MATCH>
JoinVisitor* date_join_visitor(SEXP left, SEXP right, const SymbolString& name_left, const SymbolString name_right) {
  switch (TYPEOF(left)) {
  case INTSXP:
    return date_join_visitor_right<INTSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
  case REALSXP:
    return date_join_visitor_right<REALSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
  default:
    stop("Date objects should be represented as integer or numeric");
  }
}

template <bool ACCEPT_NA_MATCH>
JoinVisitor* join_visitor(SEXP left, SEXP right, const SymbolString& name_left, const SymbolString& name_right, bool warn_) {
  // handle Date separately
  bool lhs_date = Rf_inherits(left, "Date");
  bool rhs_date = Rf_inherits(right, "Date");

  switch (lhs_date + rhs_date) {
  case 2:
    return date_join_visitor<ACCEPT_NA_MATCH>(left, right, name_left, name_right);
  case 1:
    stop("cannot join a Date object with an object that is not a Date object");
  case 0:
    break;
  default:
    break;
  }

  bool lhs_time = Rf_inherits(left, "POSIXct");
  bool rhs_time = Rf_inherits(right, "POSIXct");
  switch (lhs_time + rhs_time) {
  case 2:
    return new POSIXctJoinVisitor<ACCEPT_NA_MATCH>(left, right, name_left, name_right);
  case 1:
    stop("cannot join a POSIXct object with an object that is not a POSIXct object");
  case 0:
    break;
  default:
    break;
  }

  switch (TYPEOF(left)) {
  case CPLXSXP:
  {
    switch (TYPEOF(right)) {
    case CPLXSXP:
      return new JoinVisitorImpl<CPLXSXP, CPLXSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
    default:
      break;
    }
    break;
  }
  case INTSXP:
  {
    bool lhs_factor = Rf_inherits(left, "factor");
    switch (TYPEOF(right)) {
    case INTSXP:
    {
      bool rhs_factor = Rf_inherits(right, "factor");
      if (lhs_factor && rhs_factor) {
        if (same_levels(left, right)) {
          return new JoinVisitorImpl<INTSXP, INTSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
        } else {
          warn_bad_var(
            name_left, name_right,
            "joining factors with different levels, coercing to character vector",
            warn_
          );
          return new JoinVisitorImpl<STRSXP, STRSXP, ACCEPT_NA_MATCH>(reencode_char(left), reencode_char(right), name_left, name_right);
        }
      } else if (!lhs_factor && !rhs_factor) {
        return new JoinVisitorImpl<INTSXP, INTSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
      }
      break;
    }
    case REALSXP:
    {
      if (!lhs_factor && is_bare_vector(right)) {
        return new JoinVisitorImpl<INTSXP, REALSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
      }
      break;
      // what else: perhaps we can have INTSXP which is a Date and REALSXP which is a Date too ?
    }
    case LGLSXP:
    {
      if (!lhs_factor) {
        return new JoinVisitorImpl<INTSXP, LGLSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
      }
      break;
    }
    case STRSXP:
    {
      if (lhs_factor) {
        warn_bad_var(
          name_left, name_right,
          "joining factor and character vector, coercing into character vector",
          warn_
        );
        return new JoinVisitorImpl<STRSXP, STRSXP, ACCEPT_NA_MATCH>(reencode_char(left), reencode_char(right), name_left, name_right);
      }
    }
    default:
      break;
    }
    break;
  }
  case REALSXP:
  {
    switch (TYPEOF(right)) {
    case REALSXP:
      return new JoinVisitorImpl<REALSXP, REALSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
    case INTSXP:
      return new JoinVisitorImpl<REALSXP, INTSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
    default:
      break;
    }

  }
  case LGLSXP:
  {
    switch (TYPEOF(right)) {
    case LGLSXP:
      return new JoinVisitorImpl<LGLSXP, LGLSXP, ACCEPT_NA_MATCH> (left, right, name_left, name_right);
    case INTSXP:
      return new JoinVisitorImpl<LGLSXP, INTSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
    case REALSXP:
      return new JoinVisitorImpl<LGLSXP, REALSXP, ACCEPT_NA_MATCH>(left, right, name_left, name_right);
    default:
      break;
    }
    break;
  }
  case STRSXP:
  {
    switch (TYPEOF(right)) {
    case INTSXP:
    {
      if (Rf_inherits(right, "factor")) {
        warn_bad_var(
          name_left, name_right,
          "joining character vector and factor, coercing into character vector",
          warn_
        );
        return new JoinVisitorImpl<STRSXP, STRSXP, ACCEPT_NA_MATCH>(reencode_char(left), reencode_char(right), name_left, name_right);
      }
      break;
    }
    case STRSXP:
    {
      return new JoinVisitorImpl<STRSXP, STRSXP, ACCEPT_NA_MATCH>(reencode_char(left), reencode_char(right), name_left, name_right);
    }
    default:
      break;
    }
    break;
  }
  default:
    break;
  }

  stop("Can't join on '%s' x '%s' because of incompatible types (%s / %s)",
       name_left.get_utf8_cstring(), name_right.get_utf8_cstring(), get_single_class(left), get_single_class(right)
      );
  return 0;
}

JoinVisitor* join_visitor(SEXP left, SEXP right, const SymbolString& left_name, const SymbolString& right_name, bool warn, bool accept_na_match) {
  if (accept_na_match)
    return join_visitor<true>(left, right, left_name, right_name, warn);
  else
    return join_visitor<false>(left, right, left_name, right_name, warn);
}

}
