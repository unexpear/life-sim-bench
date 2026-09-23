// Incremental sorting: one comparison, swap, array copy, key read or bead drop per event.
// No precomputed animation tape. Auxiliary storage is O(n) except Gravity, whose
// abacus is n rows by the key range.
// Algorithm references: https://algs4.cs.princeton.edu/20sorting/
#pragma once
#include "../rng.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench::sorting {
// Appended in this order so an algorithm's index never changes meaning: saved
// setups, dedicated runs and tests all name algorithms by position.
enum class Algorithm { Bubble, Selection, Insertion, Shell, Merge, Quick, Heap,
    Cocktail, OddEven, Comb, Gnome, Cycle, Pancake, Bitonic, OddEvenMerge, RadixLSD, RadixMSD, Gravity };
inline constexpr const char* names[] = {"Bubble", "Selection", "Insertion", "Shell", "Merge", "Quick (3-way)", "Heap",
    "Cocktail shaker", "Odd-even", "Comb", "Gnome", "Cycle", "Pancake", "Bitonic", "Odd-even merge",
    "Radix LSD (base 10)", "Radix MSD (base 10)", "Gravity (bead)"};
inline constexpr int algorithmCount = int(std::size(names));
// Equal keys keep their input order.
inline constexpr bool stable(Algorithm a) {
    switch(a) {
        case Algorithm::Bubble: case Algorithm::Insertion: case Algorithm::Merge: case Algorithm::Cocktail:
        case Algorithm::OddEven: case Algorithm::Gnome: case Algorithm::RadixLSD: case Algorithm::RadixMSD: return true;
        default: return false;
    }
}
// The comparisons made do not depend on the data: a sorting network.
inline constexpr bool network(Algorithm a) {return a==Algorithm::Bitonic || a==Algorithm::OddEvenMerge;}
// Gravity's abacus is n rows by the key range. Past this many cells it refuses.
inline constexpr std::int64_t maxBeadCells = std::int64_t(1) << 24;

struct Item {
    int value = 0, identity = 0;
    bool operator==(const Item&) const = default;
};
inline std::vector<Item> dataset(int n, int pattern, unsigned seed) {
    n = std::clamp(n, 0, 512);
    std::vector<Item> a(n);
    for(int i=0;i<n;++i) a[i] = {i+1, i};
    Rng rng(mix_seed(0x534f5254, int(seed)));
    if(pattern == 1) std::reverse(a.begin(), a.end());
    else if(pattern == 2 && n > 1) {
        for(int k=0;k<std::max(1,n/16);++k) {
            const int i=int(rng.below(n-1)); std::swap(a[i],a[i+1]);
        }
    } else if(pattern == 0 || pattern == 3) {
        if(pattern == 3) for(int i=0;i<n;++i) a[i].value = (i%8+1)*std::max(1,n/8);
        for(int i=n-1;i>0;--i) std::swap(a[i], a[rng.below(i+1)]);
    }
    // Identity is the original position in THIS input, including duplicate keys.
    for(int i=0;i<n;++i) a[i].identity=i;
    return a;
}

class Engine {
public:
    std::vector<Item> values;
    std::uint64_t comparisons=0, swaps=0, writes=0, auxiliaryWrites=0, events=0;
    // Copy events (one item into one slot), key reads with no comparison, and bead-rod drops.
    std::uint64_t copies=0, reads=0, drops=0;
    int activeA=-1, activeB=-1;
    int comparedA=0, comparedB=0;
    bool auxiliaryEvent=false;
    enum class Event { Ready, Compare, Swap, Copy, Read, Drop, Complete };
    Event event=Event::Ready;
    bool done=true;
    Algorithm algorithm=Algorithm::Quick;
    bool descending=false;
    // Cycle sort holds one item out of the array while it finds that item's place.
    Item held;
    bool holding=false;
    int activeLevel=-1;          // Gravity: the rod that last dropped

    void reset(std::vector<Item> input, Algorithm method, bool reverse=false) {
        *this=Engine{};
        values=std::move(input); algorithm=method; descending=reverse;
        n=int(values.size()); done=n<2;
        if(done) event=Event::Complete;
        end=n; i=method==Algorithm::Selection?0:1; j=method==Algorithm::Selection?1:0;
        gap=1;
        if(method==Algorithm::Shell) {while(gap<n/3)gap=3*gap+1; i=j=gap;}
        else if(method==Algorithm::Insertion) j=1;
        if(method==Algorithm::Merge||method==Algorithm::RadixLSD||method==Algorithm::RadixMSD) aux.resize(n);
        if(method==Algorithm::Quick && !done) ranges.push_back({0,n-1});
        heapBuild=n/2-1;
        shaker={0,n-1,0,true,false};
        combing={n,n,false};
        pancakes.size=n;
        if(method==Algorithm::Bitonic && !done) frames.push_back({false,0,n,true,0,0});
        // The finished order, for displaying how far each item is from home.
        // Bookkeeping for the views only: it is not part of any algorithm's work.
        target.resize(values.size());
        std::transform(values.begin(),values.end(),target.begin(),[](const Item& it){return it.value;});
        std::sort(target.begin(),target.end());
        if(descending) std::reverse(target.begin(),target.end());
    }
    bool next() {
        if(done) return false;
        activeA=activeB=-1;
        auxiliaryEvent=false;
        activeLevel=-1;
        if(swapA>=0) {
            activeA=swapA; activeB=swapB;
            std::swap(values[swapA],values[swapB]);
            if(pivotIndex==swapA) pivotIndex=swapB; else if(pivotIndex==swapB) pivotIndex=swapA;
            swapA=-1; ++swaps; writes+=2; event=Event::Swap; ++events; return true;
        }
        bool emitted=false;
        switch(algorithm) {
            case Algorithm::Bubble: emitted=bubble(); break;
            case Algorithm::Selection: emitted=selection(); break;
            case Algorithm::Insertion: case Algorithm::Shell: emitted=insertion(); break;
            case Algorithm::Merge: emitted=merge(); break;
            case Algorithm::Quick: emitted=quick(); break;
            case Algorithm::Heap: emitted=heap(); break;
            case Algorithm::Cocktail: emitted=cocktail(); break;
            case Algorithm::OddEven: emitted=oddEven(); break;
            case Algorithm::Comb: emitted=comb(); break;
            case Algorithm::Gnome: emitted=gnome(); break;
            case Algorithm::Cycle: emitted=cycle(); break;
            case Algorithm::Pancake: emitted=pancake(); break;
            case Algorithm::Bitonic: emitted=bitonic(); break;
            case Algorithm::OddEvenMerge: emitted=oddEvenMerge(); break;
            case Algorithm::RadixLSD: emitted=radixLSD(); break;
            case Algorithm::RadixMSD: emitted=radixMSD(); break;
            case Algorithm::Gravity: emitted=gravity(); break;
        }
        if(emitted) ++events;
        else {done=true; event=Event::Complete; activeA=activeB=-1; activeLevel=-1; holding=false;}
        return emitted;
    }
    void advance(int budget) {for(int k=0;k<budget && !done;++k) next();}
    bool ordered() const {
        for(std::size_t k=1;k<values.size();++k) if(order(values[k-1].value,values[k].value)>0)return false;
        return true;
    }
    double adjacentOrder() const {
        if(values.size()<2)return 1;
        int good=0;for(std::size_t k=1;k<values.size();++k)good+=order(values[k-1].value,values[k].value)<=0;
        return double(good)/double(values.size()-1);
    }
    // Where the item at `at` belongs once sorted: [first, last] positions its key
    // occupies. A key the finished array does not contain (Gravity's rows pass
    // through such values) belongs in the gap where it would be inserted, between
    // `first` and `last`, and is never home: false is returned for it.
    bool home(int at, int& first, int& last) const {
        const int v=values[std::size_t(at)].value;
        const auto range=descending?std::equal_range(target.begin(),target.end(),v,std::greater<int>())
                                   :std::equal_range(target.begin(),target.end(),v);
        first=int(range.first-target.begin()); last=int(range.second-target.begin())-1;
        if(first<=last) return true;
        std::swap(first,last); first=std::max(first,0); last=std::min(last,int(target.size())-1);
        return false;
    }
    // How many positions an item is from where it belongs, counted along the array.
    int displacement(int at) const {
        int first=0,last=0;
        const bool present=home(at,first,last);
        const int d=at<first?first-at:at>last?at-last:0;
        return present?d:std::max(d,1);
    }
    // The same, counted the shorter way round, as if the array's ends were joined.
    int circularDisplacement(int at) const {
        int first=0,last=0;
        const bool present=home(at,first,last);
        const int n=int(values.size());
        const auto around=[n](int a,int b){const int d=std::abs(a-b);return std::min(d,n-d);};
        const int d=at>=first&&at<=last?0:std::min(around(at,first),around(at,last));
        return present?d:std::max(d,1);
    }
    // The smallest and largest keys of the input, which every later state shares.
    int minKey() const {return target.empty()?0:descending?target.back():target.front();}
    int maxKey() const {return target.empty()?0:descending?target.front():target.back();}
    // Mean displacement as a fraction of the array: 0 when sorted.
    double meanDisplacement() const {
        if(values.size()<2) return 0;
        double total=0; for(int k=0;k<int(values.size());++k) total+=displacement(k);
        return total/double(values.size())/double(values.size()-1);
    }
    const char* eventName() const {
        switch(event) {case Event::Compare:return "COMPARE";case Event::Swap:return "SWAP";
            case Event::Copy:return "COPY";case Event::Read:return "READ";case Event::Drop:return "DROP";
            case Event::Complete:return "SORTED";default:return "READY";}
    }
    // One line describing the latest event, in the canvas's alphabet.
    std::string detail() const {
        const auto num=[](long long v){return std::to_string(v);};
        switch(event) {
            case Event::Compare:
                if(holding) return "HELD VALUE "+num(held.value)+" AGAINST "+num(comparedA);
                return std::string(auxiliaryEvent?"BUFFER ":"")+"VALUES "+num(comparedA)+" AND "+num(comparedB)+" - "+(descending?"DESCENDING":"ASCENDING");
            case Event::Read:
                if(algorithm==Algorithm::Gravity && rx.stage==5) return "ROW "+num(activeA)+": LAY "+num(lastBeads)+" BEADS FOR VALUE "+num(lastRead);
                if(rx.stage>=1 && rx.stage<=4) return "VALUE "+num(lastRead)+" - DIGIT "+num(lastDigit)+" AT PLACE "+num(rx.place);
                return "READ VALUE "+num(lastRead)+" - FINDING THE KEY RANGE";
            case Event::Drop: return "ROD "+num(activeLevel+1)+" OF "+num(levels)+": "+num(lastSettled)+" BEADS SETTLE";
            case Event::Swap:
                if(algorithm==Algorithm::Pancake) return "FLIP POSITIONS 0 TO "+num(flipEnd);
                return "SWAP POSITIONS "+num(activeA)+" AND "+num(activeB);
            case Event::Copy:
                if(auxiliaryEvent) return "COPY INTO BUFFER POSITION "+num(activeA);
                if(algorithm==Algorithm::Cycle) return "PLACE VALUE "+num(values[std::size_t(activeA)].value)+" AT "+num(activeA)+(holding?" - NOW HOLDING "+num(held.value):"");
                return "COPY TO POSITION "+num(activeA);
            case Event::Complete: return "SORT COMPLETE - REPLAY RUNS THE SAME DATA";
            default: return "AMBER COMPARE - CORAL WRITE - GREEN SORTED";
        }
    }
    const std::vector<Item>& auxiliary() const {return aux;}
    bool usesBuffer() const {return algorithm==Algorithm::Merge||algorithm==Algorithm::RadixLSD||algorithm==Algorithm::RadixMSD;}
    // Gravity's abacus: rows are array positions, rods are bead levels above the lowest key.
    bool abacusShown() const {return algorithm==Algorithm::Gravity && rx.stage>=5 && levels>0;}
    // Rows before this one hold beads; later rows still hold only their key.
    // None are laid while the keys are still being read for their range.
    int beadsLaid() const {return rx.stage<5?0:rx.stage==5?rx.at:n;}
    int beadLevels() const {return levels;}
    long long beadBase() const {return rx.lowest;}
    bool bead(int row,int level) const {return beads[std::size_t(row)*std::size_t(levels)+std::size_t(level)]!=0;}
private:
    int n=0,i=0,j=0,end=0,best=0,gap=1,swapA=-1,swapB=-1;
    bool changed=false;
    int width=1,left=0,mid=0,right=0,p=0,q=0,k=0,stage=0,chosen=0;
    std::vector<Item> aux;
    std::vector<std::pair<int,int>> ranges;
    int lo=0,hi=-1,lt=0,gt=0,pivot=0,pivotIndex=-1;
    bool partition=false;
    int heapBuild=-1,root=-1,child=0,heapStage=0;
    std::vector<int> target;

    struct Shaker {int lo=0,hi=0,at=0; bool forward=true,changed=false;} shaker;
    struct OddEvenState {int phase=1,at=1; bool changed=false;} oddEvenState;
    struct Combing {int gap=0,at=0; bool clean=false;} combing;
    int gnomeAt=1;
    struct CycleState {int stage=0,start=0,pos=0,scan=0; bool placed=false;} cycling;
    struct Pancakes {int stage=0,size=0,scan=0,best=0,lo=0,hi=0;} pancakes;
    int flipEnd=0;
    struct Frame {bool merge; int lo,len; bool up; int stage,at;};
    std::vector<Frame> frames;
    struct Batcher {int p=1,k=1,j=0,at=0;} batcher;
    struct Distribution {
        int stage=0,at=0,lo=0,hi=0;
        long long lowest=0,highest=0,place=1;
        // Radix digits are the key's own decimal digits; keys are shifted only
        // when some are negative, so that every shifted key is at least zero.
        long long base=0;
        std::array<int,10> count{},next{},start{};
    } rx;
    struct Span {int lo,hi; long long place;};
    std::vector<Span> spans;
    std::vector<std::uint8_t> beads;
    std::vector<int> rowBeads;
    int levels=0,rod=0,lastSettled=0,lastBeads=0,lastRead=0,lastDigit=0;

    int order(int a,int b) const {return descending?(a>b?-1:a<b?1:0):(a<b?-1:a>b?1:0);}
    int compare(int a,int b) {
        activeA=a;activeB=b;event=Event::Compare;++comparisons;
        comparedA=values[a].value;comparedB=values[b].value;
        return order(values[a].value,values[b].value);
    }
    void exchangeLater(int a,int b) {if(a!=b){swapA=a;swapB=b;}}
    void swapNow(int a,int b) {activeA=a;activeB=b;std::swap(values[a],values[b]);++swaps;writes+=2;event=Event::Swap;}
    // The array key against the held item: negative when the array key belongs first.
    int compareHeld(int at) {
        activeA=at;activeB=-1;event=Event::Compare;++comparisons;
        comparedA=values[at].value;comparedB=held.value;
        return order(values[at].value,held.value);
    }
    void readKey(int at) {activeA=at;activeB=-1;event=Event::Read;++reads;lastRead=values[at].value;}
    void copied(int at,bool intoBuffer) {
        activeA=at;activeB=-1;event=Event::Copy;++copies;
        if(intoBuffer) {auxiliaryEvent=true;++auxiliaryWrites;} else ++writes;
    }
    bool bubble() {
        for(;;) {
            if(j>=end-1) {if(!changed || --end<=1)return false;j=0;changed=false;}
            if(compare(j,j+1)>0) {exchangeLater(j,j+1);changed=true;}
            ++j;return true;
        }
    }
    bool selection() {
        for(;;) {
            if(i>=n-1)return false;
            if(j<n) {if(compare(j,best)<0)best=j;++j;return true;}
            if(best!=i) {
                // A swap is itself the next event, without a fictitious comparison.
                activeA=i;activeB=best;std::swap(values[i],values[best]);++swaps;writes+=2;event=Event::Swap;
                ++i;best=i;j=i+1;return true;
            }
            ++i;best=i;j=i+1;
        }
    }
    bool insertion() {
        for(;;) {
            if(i>=n) {if(gap==1)return false;gap/=3;i=j=gap;}
            if(j<gap) {j=++i;continue;}
            if(compare(j-gap,j)>0) {exchangeLater(j-gap,j);j-=gap;}
            else j=++i;
            return true;
        }
    }
    bool merge() {
        for(;;) {
            if(width>=n)return false;
            if(stage==0) {
                if(left+width>=n) {width*=2;left=0;continue;}
                mid=left+width;right=std::min(n,left+2*width);k=left;stage=1;
            }
            if(stage==1) {
                if(k<right) {aux[k]=values[k];activeA=k++;event=Event::Copy;auxiliaryEvent=true;++auxiliaryWrites;++copies;return true;}
                p=left;q=mid;k=left;stage=2;
            }
            if(stage==2) {
                if(k>=right) {left+=2*width;stage=0;continue;}
                if(p<mid && q<right) {
                    activeA=p;activeB=q;event=Event::Compare;++comparisons;
                    auxiliaryEvent=true;comparedA=aux[p].value;comparedB=aux[q].value;
                    // Prefer the left item on equality: stable in both directions.
                    chosen=order(aux[p].value,aux[q].value)<=0?p++:q++;
                    stage=3;return true;
                }
                chosen=p<mid?p++:q++;stage=3;
            }
            values[k]=aux[chosen];activeA=k++;activeB=-1;event=Event::Copy;++writes;++copies;stage=2;return true;
        }
    }
    bool quick() {
        for(;;) {
            if(!partition) {
                if(ranges.empty())return false;
                auto range=ranges.back();ranges.pop_back();lo=range.first;hi=range.second;
                if(lo>=hi)continue;
                pivotIndex=lo+(hi-lo)/2;pivot=values[pivotIndex].value;
                lt=i=lo;gt=hi;partition=true;
            }
            if(i>gt) {
                if(lo<lt-1)ranges.push_back({lo,lt-1});
                if(gt+1<hi)ranges.push_back({gt+1,hi});
                partition=false;continue;
            }
            activeA=i;activeB=pivotIndex;event=Event::Compare;++comparisons;
            comparedA=values[i].value;comparedB=pivot;
            const int cmp=order(values[i].value,pivot);
            if(cmp<0) {exchangeLater(i,lt);++i;++lt;}
            else if(cmp>0) {exchangeLater(i,gt);--gt;}
            else ++i;
            return true;
        }
    }
    bool heap() {
        for(;;) {
            if(root<0) {
                if(heapBuild>=0)root=heapBuild--;
                else {
                    if(end<=1)return false;
                    activeA=0;activeB=--end;std::swap(values[0],values[end]);
                    ++swaps;writes+=2;event=Event::Swap;root=0;heapStage=0;return true;
                }
                heapStage=0;
            }
            if(heapStage==0) {
                child=2*root+1;
                if(child>=end) {root=-1;continue;}
                heapStage=1;
                if(child+1<end) {if(compare(child,child+1)<0)++child;return true;}
            }
            if(compare(root,child)<0) {exchangeLater(root,child);root=child;heapStage=0;}
            else root=-1;
            return true;
        }
    }
    // Bubble passes in both directions: forward carries the last item home, backward the first.
    bool cocktail() {
        for(;;) {
            auto& s=shaker;
            if(s.lo>=s.hi) return false;
            if(s.forward) {
                if(s.at<s.hi) {
                    if(compare(s.at,s.at+1)>0) {exchangeLater(s.at,s.at+1);s.changed=true;}
                    ++s.at;return true;
                }
                if(!s.changed) return false;
                --s.hi;s.forward=false;s.changed=false;s.at=s.hi;
            } else {
                if(s.at>s.lo) {
                    if(compare(s.at-1,s.at)>0) {exchangeLater(s.at-1,s.at);s.changed=true;}
                    --s.at;return true;
                }
                if(!s.changed) return false;
                ++s.lo;s.forward=true;s.changed=false;s.at=s.lo;
            }
        }
    }
    // Brick sort: odd-indexed pairs, then even-indexed pairs, until a round of both exchanges nothing.
    bool oddEven() {
        for(;;) {
            auto& s=oddEvenState;
            if(s.at+1<n) {
                if(compare(s.at,s.at+1)>0) {exchangeLater(s.at,s.at+1);s.changed=true;}
                s.at+=2;return true;
            }
            if(s.phase==1) {s.phase=0;s.at=0;continue;}
            if(!s.changed) return false;
            s.phase=1;s.at=1;s.changed=false;
        }
    }
    // Bubble passes over a gap that shrinks by 1.3 each pass; a clean pass at gap 1 finishes.
    bool comb() {
        for(;;) {
            auto& s=combing;
            if(s.at+s.gap<n) {
                if(compare(s.at,s.at+s.gap)>0) {exchangeLater(s.at,s.at+s.gap);s.clean=false;}
                ++s.at;return true;
            }
            if(s.gap==1 && s.clean) return false;
            s.gap=std::max(1,s.gap*10/13);s.at=0;s.clean=true;
        }
    }
    // Step forward past ordered pairs; exchange a disordered pair and step back.
    bool gnome() {
        if(gnomeAt>=n) return false;
        if(compare(gnomeAt-1,gnomeAt)>0) {exchangeLater(gnomeAt-1,gnomeAt);if(--gnomeAt==0)gnomeAt=1;}
        else ++gnomeAt;
        return true;
    }
    // Take an item out, count the keys that belong before it, and write it there,
    // taking out the item it displaces. Each misplaced item is written once.
    bool cycle() {
        auto& s=cycling;
        for(;;) switch(s.stage) {
            case 0:
                if(s.start>=n-1) {holding=false;return false;}
                held=values[std::size_t(s.start)];holding=true;
                s.pos=s.start;s.scan=s.start+1;s.placed=false;s.stage=1;continue;
            case 1:
                if(s.scan<n) {if(compareHeld(s.scan)<0)++s.pos;++s.scan;return true;}
                if(!s.placed && s.pos==s.start) {holding=false;++s.start;s.stage=0;continue;}
                s.stage=2;continue;
            case 2:
                // Skip past keys equal to the held one, so duplicates land side by side.
                if(s.pos>=n) throw std::logic_error("cycle sort counted a position past the array");
                if(compareHeld(s.pos)==0) {++s.pos;return true;}
                s.stage=3;return true;
            case 3: {
                const int to=s.pos;
                std::swap(values[std::size_t(to)],held);s.placed=true;
                if(to==s.start) {holding=false;++s.start;s.stage=0;}
                else {s.pos=s.start;s.scan=s.start+1;s.stage=1;}
                copied(to,false);
                return true;
            }
            default: return false;
        }
    }
    // Find the item that belongs last in the unsorted prefix, flip it to the front, then flip the prefix.
    bool pancake() {
        auto& s=pancakes;
        for(;;) switch(s.stage) {
            case 0:
                if(s.size<2) return false;
                s.best=0;s.scan=1;s.stage=1;continue;
            case 1:
                // >= keeps the LAST of equal largest keys, so one already at the end needs no flip.
                if(s.scan<s.size) {if(compare(s.scan,s.best)>=0)s.best=s.scan;++s.scan;return true;}
                if(s.best==s.size-1) {--s.size;s.stage=0;continue;}
                s.lo=0;
                if(s.best==0) {s.hi=s.size-1;s.stage=3;} else {s.hi=s.best;s.stage=2;}
                flipEnd=s.hi;continue;
            case 2: case 3:
                if(s.lo<s.hi) {const int a=s.lo++,b=s.hi--;swapNow(a,b);return true;}
                if(s.stage==2) {s.lo=0;s.hi=s.size-1;flipEnd=s.hi;s.stage=3;continue;}
                --s.size;s.stage=0;continue;
            default: return false;
        }
    }
    // Bitonic sort for any n (H. W. Lang): sort the halves in opposite directions,
    // then merge with a half-cleaner spanning the largest power of two below the length.
    bool bitonic() {
        while(!frames.empty()) {
            const Frame f=frames.back();
            if(f.len<2 || f.stage==3) {frames.pop_back();continue;}
            if(!f.merge) {
                const int half=f.len/2;
                frames.back().stage=f.stage+1;
                if(f.stage==0) frames.push_back({false,f.lo,half,!f.up,0,0});
                else if(f.stage==1) frames.push_back({false,f.lo+half,f.len-half,f.up,0,0});
                else frames.push_back({true,f.lo,f.len,f.up,0,0});
                continue;
            }
            int span=1;while(span*2<f.len)span*=2;
            if(f.stage==0) {
                if(f.at<f.len-span) {
                    const int a=f.lo+f.at,b=a+span;
                    ++frames.back().at;
                    const int c=compare(a,b);
                    if(f.up?c>0:c<0) exchangeLater(a,b);
                    return true;
                }
                frames.back().stage=1;
                frames.push_back({true,f.lo,span,f.up,0,0});
                continue;
            }
            frames.back().stage=f.stage+1;
            if(f.stage==1) frames.push_back({true,f.lo+span,f.len-span,f.up,0,0});
        }
        return false;
    }
    // Batcher's odd-even merge sort, the iterative network for any n.
    bool oddEvenMerge() {
        auto& s=batcher;
        for(;;) {
            if(s.p>=n) return false;
            if(s.k<1) {s.p*=2;s.k=s.p;s.j=0;s.at=0;continue;}
            if(s.j>n-1-s.k) {s.k/=2;if(s.k>=1)s.j=s.k%s.p;s.at=0;continue;}
            if(s.at>std::min(s.k-1,n-s.j-s.k-1)) {s.j+=2*s.k;s.at=0;continue;}
            const int a=s.at+s.j,b=a+s.k;
            ++s.at;
            if(a/(2*s.p)!=b/(2*s.p)) continue;
            if(compare(a,b)>0) exchangeLater(a,b);
            return true;
        }
    }
    // Radix and Gravity work on keys relative to the lowest one, found by reading every key.
    bool scanRange() {
        if(rx.at>=n) return false;
        readKey(rx.at);
        const long long v=values[std::size_t(rx.at)].value;
        if(rx.at==0) rx.lowest=rx.highest=v;
        else {rx.lowest=std::min(rx.lowest,v);rx.highest=std::max(rx.highest,v);}
        rx.base=std::min(rx.lowest,0LL);
        ++rx.at;return true;
    }
    int bucketOf(const Item& it) const {
        const int digit=int(((it.value-rx.base)/rx.place)%10);
        return descending?9-digit:digit;
    }
    void beginDistribution(int from,int to) {rx.lo=from;rx.hi=to;rx.at=from;rx.count.fill(0);rx.stage=1;}
    // One stable counting pass over [rx.lo, rx.hi) on the digit at rx.place:
    // read every digit, copy each item into its bucket in the buffer, copy the buffer back.
    // False once the range has been copied back.
    bool distribute() {
        if(rx.stage==1) {
            if(rx.at<rx.hi) {
                readKey(rx.at);
                lastDigit=int(((lastRead-rx.base)/rx.place)%10);
                ++rx.count[std::size_t(bucketOf(values[std::size_t(rx.at)]))];++rx.at;return true;
            }
            int total=rx.lo;
            for(std::size_t b=0;b<10;++b) {rx.start[b]=rx.next[b]=total;total+=rx.count[b];}
            rx.stage=2;rx.at=rx.lo;
        }
        if(rx.stage==2) {
            if(rx.at<rx.hi) {
                const int to=rx.next[std::size_t(bucketOf(values[std::size_t(rx.at)]))]++;
                aux[std::size_t(to)]=values[std::size_t(rx.at)];++rx.at;
                copied(to,true);return true;
            }
            rx.stage=3;rx.at=rx.lo;
        }
        if(rx.at<rx.hi) {values[std::size_t(rx.at)]=aux[std::size_t(rx.at)];copied(rx.at,false);++rx.at;return true;}
        return false;
    }
    // Least significant digit first; each pass is stable, so earlier digits break ties.
    bool radixLSD() {
        for(;;) {
            if(rx.stage==0) {
                if(scanRange()) return true;
                if(rx.highest==rx.lowest) return false;
                rx.place=1;beginDistribution(0,n);
            }
            if(distribute()) return true;
            if((rx.highest-rx.base)/rx.place<10) return false;
            rx.place*=10;beginDistribution(0,n);
        }
    }
    // Most significant digit first: distribute a range, then each bucket of two or more on the next digit.
    bool radixMSD() {
        for(;;) {
            if(rx.stage==0) {
                if(scanRange()) return true;
                if(rx.highest==rx.lowest) return false;
                long long top=1;while((rx.highest-rx.base)/top>=10)top*=10;
                spans.push_back({0,n,top});rx.stage=4;
            }
            if(rx.stage==4) {
                if(spans.empty()) return false;
                const Span s=spans.back();spans.pop_back();
                rx.place=s.place;beginDistribution(s.lo,s.hi);
            }
            if(distribute()) return true;
            // Lower buckets pushed last, so they are taken first.
            if(rx.place>1)
                for(int b=9;b>=0;--b) if(rx.count[std::size_t(b)]>=2)
                    spans.push_back({rx.start[std::size_t(b)],rx.start[std::size_t(b)]+rx.count[std::size_t(b)],rx.place/10});
            rx.stage=4;
        }
    }
    // Bead sort. Row r holds (key - lowest) beads on rods 0, 1, ...; each rod's beads
    // fall to the end of the array (the front, for descending), and a row's value is
    // the lowest key plus its beads. Rows are rebuilt, not moved, so items are not carried.
    bool gravity() {
        for(;;) switch(rx.stage) {
            case 0: {
                if(scanRange()) return true;
                if(rx.highest==rx.lowest) return false;
                const long long range=rx.highest-rx.lowest;
                if(range>maxBeadCells/n) throw std::length_error("gravity sort would need "+std::to_string(range*n)+" abacus cells");
                levels=int(range);
                beads.assign(std::size_t(n)*std::size_t(levels),0);rowBeads.assign(std::size_t(n),0);
                rx.stage=5;rx.at=0;continue;
            }
            case 5:
                if(rx.at<n) {
                    readKey(rx.at);
                    lastBeads=int(lastRead-rx.lowest);
                    for(int l=0;l<lastBeads;++l) beads[std::size_t(rx.at)*std::size_t(levels)+std::size_t(l)]=1;
                    rowBeads[std::size_t(rx.at)]=lastBeads;auxiliaryWrites+=std::uint64_t(lastBeads);
                    ++rx.at;return true;
                }
                for(auto& item:values) item.identity=-1;
                rx.stage=6;rod=0;continue;
            case 6: {
                if(rod>=levels) return false;
                int count=0;
                for(int r=0;r<n;++r) count+=bead(r,rod);
                std::uint64_t changes=0;
                for(int r=0;r<n;++r) {
                    const std::uint8_t now=descending?r<count:r>=n-count;
                    auto& cell=beads[std::size_t(r)*std::size_t(levels)+std::size_t(rod)];
                    if(now==cell) continue;
                    cell=now;rowBeads[std::size_t(r)]+=now?1:-1;
                    values[std::size_t(r)].value=int(rx.lowest+rowBeads[std::size_t(r)]);
                    ++changes;
                }
                writes+=changes;auxiliaryWrites+=changes;++drops;
                activeLevel=rod;lastSettled=count;event=Event::Drop;++rod;
                return true;
            }
            default: return false;
        }
    }
};
} // namespace bench::sorting
