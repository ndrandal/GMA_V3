#include "gma/ob/ObKey.hpp"
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace gma::ob {
namespace {

std::vector<std::string> split(const std::string& s, char sep='.') {
  std::vector<std::string> out; std::string cur; cur.reserve(16);
  for(char c: s){ if(c==sep){ if(!cur.empty()) out.push_back(cur); cur.clear(); } else cur.push_back(c); }
  if(!cur.empty()) out.push_back(cur);
  return out;
}
std::string lower(std::string v){ std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){return std::tolower(c);}); return v; }

bool parseInt(const std::string& s, int& out){
  char* end=nullptr; long v=strtol(s.c_str(), &end, 10); if(!end || *end!='\0') return false; out=int(v); return true;
}
bool parseDouble(const std::string& s, double& out){
  char* end=nullptr; double v=strtod(s.c_str(), &end); if(!end || *end!='\0') return false; out=v; return true;
}
bool parseSide(const std::string& s, Side& out){
  std::string t=lower(s); if(t=="bid"){out=Side::Bid; return true;} if(t=="ask"){out=Side::Ask; return true;} return false;
}
bool parseTarget(const std::string& s, Target& t){
  std::string v=lower(s);
  if(v=="price"){t=Target::Price; return true;}
  if(v=="size"){t=Target::Size; return true;}
  if(v=="orders"){t=Target::Orders; return true;}
  if(v=="notional"){t=Target::Notional; return true;}
  return false;
}
bool parseReduce(const std::string& s, Reduce& r){
  std::string v=lower(s);
  if(v=="sum"){r=Reduce::Sum; return true;}
  if(v=="avg"){r=Reduce::Avg; return true;}
  if(v=="min"){r=Reduce::Min; return true;}
  if(v=="max"){r=Reduce::Max; return true;}
  if(v=="count"){r=Reduce::Count; return true;}
  return false;
}

} // anon

bool isObKey(const std::string& keyStr){
  auto toks = split(keyStr);
  return !toks.empty() && toks[0] == "ob";
}

std::optional<ObKey> parseObKey(const std::string& keyStr){
  auto t = split(keyStr);
  if(t.empty() || t[0]!="ob") return std::nullopt;
  ObKey k;
  // optional mode suffix .per/.agg at end
  if(t.size()>=2 && (t.back()=="per" || t.back()=="agg")) {
    k.mode = (t.back()=="agg"? Mode::Agg : Mode::Per);
    t.pop_back();
  }
  if(t.size()==2 && t[1]=="spread"){ k.metric=Metric::Spread; return k; }
  if(t.size()==2 && t[1]=="mid"){ k.metric=Metric::Mid; return k; }

  // best.bid.price / best.ask.size
  if(t.size()==4 && t[1]=="best"){
    k.metric=Metric::Best;
    if(!parseSide(t[2], k.bestSide)) return std::nullopt;
    if(!parseTarget(t[3], k.bestAttr)) return std::nullopt;
    if(!(k.bestAttr==Target::Price || k.bestAttr==Target::Size)) return std::nullopt;
    return k;
  }

  // level.bid.N.(price|size|orders|notional)
  if(t.size()==5 && t[1]=="level"){
    k.metric=Metric::LevelIdx;
    if(!parseSide(t[2], k.levelIdx.side)) return std::nullopt;
    if(!parseInt(t[3], k.levelIdx.n) || k.levelIdx.n<1) return std::nullopt;
    if(!parseTarget(t[4], k.levelIdx.attr)) return std::nullopt;
    return k;
  }

  // at.bid.PRICE.(size|orders|notional|price)
  if(t.size()==5 && t[1]=="at"){
    k.metric=Metric::LevelPx;
    if(!parseSide(t[2], k.levelPx.side)) return std::nullopt;
    if(!parseDouble(t[3], k.levelPx.px)) return std::nullopt;
    if(!parseTarget(t[4], k.levelPx.attr)) return std::nullopt;
    return k;
  }

  // range.bid.levels.A-B.(sum|avg|min|max|count).(size|price|orders|notional)
  if(t.size()==8 && t[1]=="range" && t[3]=="levels"){
    k.metric=Metric::RangeIdx;
    if(!parseSide(t[2], k.rangeIdx.side)) return std::nullopt;
    auto ab = split(t[4], '-'); if(ab.size()!=2) return std::nullopt;
    if(!parseInt(ab[0], k.rangeIdx.lv.a) || !parseInt(ab[1], k.rangeIdx.lv.b)) return std::nullopt;
    if(k.rangeIdx.lv.a<1 || k.rangeIdx.lv.b<k.rangeIdx.lv.a) return std::nullopt;
    if(!parseReduce(t[5], k.rangeIdx.reduce)) return std::nullopt;
    if(k.rangeIdx.reduce!=Reduce::Count){
      if(!parseTarget(t[6], k.rangeIdx.target)) return std::nullopt;
    } else {
      k.rangeIdx.target = Target::None; // target ignored for count
    }
    return k;
  }

  // range.bid.price.P1-P2.(sum|avg|min|max|count).(size|price|orders|notional)
  if(t.size()==8 && t[1]=="range" && t[3]=="price"){
    k.metric=Metric::RangePx;
    if(!parseSide(t[2], k.rangePx.side)) return std::nullopt;
    auto ab = split(t[4], '-'); if(ab.size()!=2) return std::nullopt;
    if(!parseDouble(ab[0], k.rangePx.p1) || !parseDouble(ab[1], k.rangePx.p2)) return std::nullopt;
    if(k.rangePx.p2 < k.rangePx.p1) return std::nullopt;
    if(!parseReduce(t[5], k.rangePx.reduce)) return std::nullopt;
    if(k.rangePx.reduce!=Reduce::Count){
      if(!parseTarget(t[6], k.rangePx.target)) return std::nullopt;
    } else {
      k.rangePx.target = Target::None;
    }
    return k;
  }

  // cum.bid.levels.N.(size|notional|orders|price?)
  if(t.size()==6 && t[1]=="cum" && t[3]=="levels"){
    k.metric=Metric::Cum;
    if(!parseSide(t[2], k.cumSide)) return std::nullopt;
    if(!parseInt(t[4], k.cumN) || k.cumN<1) return std::nullopt;
    if(!parseTarget(t[5], k.cumTarget)) return std::nullopt;
    return k;
  }

  // vwap.bid.levels.A-B  | vwap.bid.levels.N  | vwap.bid.price.P1-P2
  if(t.size()>=5 && t[1]=="vwap"){
    k.metric=Metric::VWAP;
    if(!parseSide(t[2], k.vwapSide)) return std::nullopt;
    if(t[3]=="levels"){
      if(t.size()==5){ // N
        int n=0; if(!parseInt(t[4], n) || n<1) return std::nullopt;
        k.vwapLv = {1, n}; k.vwapByLevels = true; return k;
      }
      if(t.size()==6){ // A-B
        auto ab = split(t[4], '-'); if(ab.size()!=2) return std::nullopt;
        if(!parseInt(ab[0], k.vwapLv.a) || !parseInt(ab[1], k.vwapLv.b)) return std::nullopt;
        if(k.vwapLv.a<1 || k.vwapLv.b<k.vwapLv.a) return std::nullopt;
        k.vwapByLevels = true; return k;
      }
      return std::nullopt;
    }
    if(t[3]=="price" && t.size()==6){
      auto ab = split(t[4], '-'); if(ab.size()!=2) return std::nullopt;
      if(!parseDouble(ab[0], k.vwapP1) || !parseDouble(ab[1], k.vwapP2)) return std::nullopt;
      if(k.vwapP2 < k.vwapP1) return std::nullopt;
      k.vwapByLevels = false; return k;
    }
    return std::nullopt;
  }

  // imbalance.levels.A-B | imbalance.levels.N | imbalance.price.P1-P2
  if(t.size()>=4 && t[1]=="imbalance"){
    k.metric=Metric::Imbalance;
    if(t[2]=="levels"){
      if(t.size()==4){ // N
        int n=0; if(!parseInt(t[3], n) || n<1) return std::nullopt;
        k.imbLv = {1, n}; k.imbByLevels = true; return k;
      }
      if(t.size()==5){ // A-B
        auto ab = split(t[3], '-'); if(ab.size()!=2) return std::nullopt;
        if(!parseInt(ab[0], k.imbLv.a) || !parseInt(ab[1], k.imbLv.b)) return std::nullopt;
        if(k.imbLv.a<1 || k.imbLv.b<k.imbLv.a) return std::nullopt;
        k.imbByLevels = true; return k;
      }
      return std::nullopt;
    }
    if(t[2]=="price" && t.size()==5){
      auto ab = split(t[3], '-'); if(ab.size()!=2) return std::nullopt;
      if(!parseDouble(ab[0], k.imbP1) || !parseDouble(ab[1], k.imbP2)) return std::nullopt;
      if(k.imbP2 < k.imbP1) return std::nullopt;
      k.imbByLevels = false; return k;
    }
    return std::nullopt;
  }

  // meta.seq | meta.epoch | meta.is_stale | meta.levels.bid | meta.levels.ask | meta.last_change_ms
  if(t.size()>=2 && t[1].rfind("meta",0)==0){
    k.metric = Metric::Meta;
    if(t.size()==3) k.metaField = t[2];
    else if (t.size()==4 && t[2]=="levels") k.metaField = "levels."+t[3];
    else return std::nullopt;
    return k;
  }

  return std::nullopt;
}

std::string formatObKey(const ObKey& k){
  std::ostringstream oss; oss<<"ob.";
  switch(k.metric){
    case Metric::Spread: oss<<"spread"; break;
    case Metric::Mid:    oss<<"mid"; break;
    case Metric::Best:
      oss<<"best."<<toString(k.bestSide)<<"."<<(k.bestAttr==Target::Price?"price":"size"); break;
    case Metric::LevelIdx:
      oss<<"level."<<toString(k.levelIdx.side)<<"."<<k.levelIdx.n<<".";
      switch(k.levelIdx.attr){
        case Target::Price: oss<<"price"; break; case Target::Size: oss<<"size"; break;
        case Target::Orders: oss<<"orders"; break; default: oss<<"notional"; break;
      } break;
    case Metric::LevelPx:
      oss<<"at."<<toString(k.levelPx.side)<<"."<<k.levelPx.px<<".";
      switch(k.levelPx.attr){
        case Target::Price: oss<<"price"; break; case Target::Size: oss<<"size"; break;
        case Target::Orders: oss<<"orders"; break; default: oss<<"notional"; break;
      } break;
    case Metric::RangeIdx:
      oss<<"range."<<toString(k.rangeIdx.side)<<".levels."
         <<k.rangeIdx.lv.a<<"-"<<k.rangeIdx.lv.b<<".";
      switch(k.rangeIdx.reduce){
        case Reduce::Sum: oss<<"sum."; break; case Reduce::Avg: oss<<"avg."; break;
        case Reduce::Min: oss<<"min."; break; case Reduce::Max: oss<<"max."; break;
        case Reduce::Count: oss<<"count."; break; default: break;
      }
      if(k.rangeIdx.reduce!=Reduce::Count){
        switch(k.rangeIdx.target){
          case Target::Price: oss<<"price"; break; case Target::Size: oss<<"size"; break;
          case Target::Orders: oss<<"orders"; break; default: oss<<"notional"; break;
        }
      }
      break;
    case Metric::RangePx:
      oss<<"range."<<toString(k.rangePx.side)<<".price."
         <<k.rangePx.p1<<"-"<<k.rangePx.p2<<".";
      switch(k.rangePx.reduce){
        case Reduce::Sum: oss<<"sum."; break; case Reduce::Avg: oss<<"avg."; break;
        case Reduce::Min: oss<<"min."; break; case Reduce::Max: oss<<"max."; break;
        case Reduce::Count: oss<<"count."; break; default: break;
      }
      if(k.rangePx.reduce!=Reduce::Count){
        switch(k.rangePx.target){
          case Target::Price: oss<<"price"; break; case Target::Size: oss<<"size"; break;
          case Target::Orders: oss<<"orders"; break; default: oss<<"notional"; break;
        }
      }
      break;
    case Metric::Cum:
      oss<<"cum."<<toString(k.cumSide)<<".levels."<<k.cumN<<".";
      switch(k.cumTarget){
        case Target::Size: oss<<"size"; break; case Target::Notional: oss<<"notional"; break;
        case Target::Orders: oss<<"orders"; break; default: oss<<"price"; break;
      } break;
    case Metric::VWAP:
      oss<<"vwap."<<toString(k.vwapSide)<<".";
      if(k.vwapByLevels) oss<<"levels."<<k.vwapLv.a<<"-"<<k.vwapLv.b;
      else oss<<"price."<<k.vwapP1<<"-"<<k.vwapP2;
      break;
    case Metric::Imbalance:
      oss<<"imbalance.";
      if(k.imbByLevels) oss<<"levels."<<k.imbLv.a<<"-"<<k.imbLv.b;
      else oss<<"price."<<k.imbP1<<"-"<<k.imbP2;
      break;
    case Metric::Meta:
      oss<<"meta."<<k.metaField; break;
  }
  if(k.mode==Mode::Agg) oss<<".agg";
  return oss.str();
}

} // namespace gma::ob
