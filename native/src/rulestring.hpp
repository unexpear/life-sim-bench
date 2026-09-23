#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bench {
enum class RuleNeighborhood { Moore, VonNeumann, Hexagonal };
struct RuleSpec {
    std::uint16_t birth=0,survive=0;
    int states=2;
    RuleNeighborhood neighborhood=RuleNeighborhood::Moore;
    bool nonTotalistic=false;
    std::array<std::uint8_t,512> transitions{};
    std::string text,error;
    bool ok=false;
    [[nodiscard]] bool generations() const {return states>2;}
    [[nodiscard]] int neighbors() const {return neighborhood==RuleNeighborhood::Moore?8:neighborhood==RuleNeighborhood::VonNeumann?4:6;}
};

// Nine row-major bits: NW,N,NE,W,centre,E,SW,S,SE. The centre is bit 4.
// Hensel's letter assignments were checked against Golly's published diagram
// and liferules.cpp representative table. Table attribution: The Golly Gang,
// GPL-2.0-or-later; see THIRD_PARTY_NOTICES.md and licenses/Golly.txt.
// https://golly.sourceforge.io/Help/Algorithms/QuickLife.html
// https://raw.githubusercontent.com/AlephAlpha/golly/master/gollybase/liferules.cpp
namespace rulestring_detail {
inline constexpr unsigned surrounding=0x1ef;
struct HenselClass {int count;char letter;unsigned pattern;};
inline constexpr HenselClass classes[]={
    {1,'c',1},{1,'e',2},
    {2,'c',5},{2,'e',10},{2,'a',3},{2,'i',40},{2,'k',33},{2,'n',68},
    {3,'c',69},{3,'e',42},{3,'a',11},{3,'i',7},{3,'k',98},{3,'n',13},{3,'j',14},{3,'q',70},{3,'r',41},{3,'y',97},
    {4,'c',325},{4,'e',170},{4,'a',15},{4,'i',45},{4,'k',99},{4,'n',71},{4,'j',106},{4,'q',102},{4,'r',43},{4,'y',101},{4,'t',105},{4,'w',78},{4,'z',108}
};
inline unsigned transform(unsigned pattern,int turns,bool reflect) {
    unsigned out=0;
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)if(pattern&(1u<<((y+1)*3+x+1))) {
        int a=reflect?-x:x,b=y;
        for(int t=0;t<turns;++t){const int next=-b;b=a;a=next;}
        out|=1u<<((b+1)*3+a+1);
    }
    return out;
}
inline const std::array<char,512>& classification() {
    static const auto table=[] {
        std::array<char,512> result{};
        for(const auto& entry:classes)for(int flip=0;flip<2;++flip)for(int turn=0;turn<4;++turn) {
            const unsigned pattern=transform(entry.pattern,turn,flip!=0);
            result[pattern]=entry.letter;
            if(entry.count<4)result[pattern^surrounding]=entry.letter;
        }
        return result;
    }();
    return table;
}
inline std::string letters(int count) {
    std::string result;for(const auto& entry:classes)if(entry.count==std::min(count,8-count))result+=entry.letter;
    std::sort(result.begin(),result.end());return result;
}
inline unsigned neighborhood_mask(RuleNeighborhood n) {
    return n==RuleNeighborhood::Moore?surrounding:n==RuleNeighborhood::VonNeumann?170u:427u;
}
}

inline std::string rule_to_string(std::uint16_t b,std::uint16_t s,int states) {
    std::string result="B";for(int n=0;n<=8;++n)if(b&(1u<<n))result+=char('0'+n);
    result+="/S";for(int n=0;n<=8;++n)if(s&(1u<<n))result+=char('0'+n);
    if(states>2)result+="/"+std::to_string(states);
    return result;
}

inline RuleSpec parse_rule(std::string_view input) {
    using namespace rulestring_detail;
    RuleSpec r;auto fail=[&](std::string error){r.error=std::move(error);r.ok=false;return r;};
    if(input.size()>2048)return fail("Rule is too long (maximum 2048 characters).");
    std::string text;
    for(unsigned char c:input)if(!std::isspace(c))text+=char(std::tolower(c));
    if(text.empty())return fail("Enter a birth/survival rule, such as B3/S23.");
    if(text.find(':')!=std::string::npos)return fail("Grid suffixes are not supported. Set world size below; this world wraps at its edges.");
    if(text.starts_with("map")||text.starts_with('r'))return fail("MAP and larger-neighbourhood rules are not supported by this editor.");
    if(text.back()=='h'||text.back()=='v') {
        r.neighborhood=text.back()=='h'?RuleNeighborhood::Hexagonal:RuleNeighborhood::VonNeumann;
        text.pop_back();
    }
    std::vector<std::string> parts(1);
    for(char c:text){if(c=='/'||c=='_')parts.emplace_back();else parts.back()+=c;}
    if(parts.size()>3)return fail("Use birth/survival with an optional state count.");
    std::string birth,survive,state;bool haveB=false,haveS=false,haveState=false;
    const bool tagged=std::any_of(parts.begin(),parts.end(),[](const auto& p){return !p.empty()&&(p[0]=='b'||p[0]=='s'||p[0]=='c'||p[0]=='g');});
    if(tagged) {
        for(std::size_t i=0;i<parts.size();++i) {
            const auto& p=parts[i];
            if(p.empty())return fail("A rule field is empty. Use B or S for an empty birth or survival set.");
            if(p[0]=='b'){if(haveB)return fail("The birth field appears twice.");haveB=true;birth=p.substr(1);}
            else if(p[0]=='s'){if(haveS)return fail("The survival field appears twice.");haveS=true;survive=p.substr(1);}
            else if(p[0]=='c'||p[0]=='g'||i==2) {
                if(haveState)return fail("The state count appears twice.");
                haveState=true;state=(p[0]=='c'||p[0]=='g')?p.substr(1):p;
            } else return fail("Use B for birth and S for survival, or the older survival/birth form.");
        }
        if(!haveB&&!haveS)return fail("A birth or survival field is required.");
    } else {
        if(parts.size()<2)return fail("Use B3/S23 or the older survival/birth form, 23/3.");
        survive=parts[0];birth=parts[1];
        if(parts.size()==3){haveState=true;state=parts[2];}
    }
    if(haveState) {
        if(state.empty())return fail("State count is missing.");
        int count=0;for(char c:state) {
            if(c<'0'||c>'9')return fail("State count must be a whole number from 2 to 256.");
            count=count*10+(c-'0');if(count>256)return fail("State count must be between 2 and 256.");
        }
        if(count<2)return fail("State count must be between 2 and 256.");
        r.states=count;
    }
    auto clause=[&](const std::string& body,bool living,std::string& canonical)->bool {
        std::array<bool,512> allowed{};std::size_t at=0;
        while(at<body.size()) {
            const char digit=body[at++];
            if(digit<'0'||digit>'0'+r.neighbors()){r.error="Neighbour count must be between 0 and "+std::to_string(r.neighbors())+" for this neighbourhood.";return false;}
            const int count=digit-'0';bool inverse=false;
            if(at<body.size()&&body[at]=='-'){inverse=true;++at;}
            std::string chosen;
            while(at<body.size()&&body[at]>='a'&&body[at]<='z')chosen+=body[at++];
            const std::string valid=letters(count);
            if((inverse||!chosen.empty())&&(r.neighborhood!=RuleNeighborhood::Moore||count==0||count==8)){r.error="Hensel letters apply only to 1-7 neighbours in the eight-neighbour grid.";return false;}
            for(char c:chosen)if(valid.find(c)==std::string::npos){r.error=std::string("Invalid Hensel letter '")+c+"' after "+digit+".";return false;}
            for(unsigned mask=0;mask<512;++mask) {
                if(mask&16u)continue;
                if(std::popcount(mask&neighborhood_mask(r.neighborhood))!=count)continue;
                const bool selected=chosen.find(classification()[mask])!=std::string::npos;
                if(chosen.empty()||(inverse?!selected:selected))allowed[mask]=true;
            }
        }
        for(int count=0;count<=r.neighbors();++count) {
            bool any=false,all=true;std::string included,excluded;
            for(unsigned mask=0;mask<512;++mask)if(!(mask&16u)&&std::popcount(mask&neighborhood_mask(r.neighborhood))==count) {
                any|=allowed[mask];all&=allowed[mask];
                if(allowed[mask])r.transitions[mask|(living?16u:0u)]=1;
            }
            if(!any)continue;
            (living?r.survive:r.birth)|=std::uint16_t(1u<<count);
            canonical+=char('0'+count);
            if(!all) {
                r.nonTotalistic=true;
                for(char letter:letters(count)) {
                    bool accepted=false;for(unsigned mask=0;mask<512;++mask)if(classification()[mask]==letter&&std::popcount(mask)==count&&allowed[mask]){accepted=true;break;}
                    (accepted?included:excluded)+=letter;
                }
                canonical+=excluded.size()+1<included.size()?"-"+excluded:included;
            }
        }
        return true;
    };
    std::string b,s;
    if(!clause(birth,false,b)||!clause(survive,true,s))return r;
    if(r.birth&1u)return fail("B0 rules are not supported by this editor.");
    r.text="B"+b+"/S"+s;
    if(r.generations())r.text+="/"+std::to_string(r.states);
    if(r.neighborhood!=RuleNeighborhood::Moore)r.text+=r.neighborhood==RuleNeighborhood::Hexagonal?'H':'V';
    r.ok=true;return r;
}
} // namespace bench
