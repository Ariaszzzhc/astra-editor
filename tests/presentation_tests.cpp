#include <astra/ui/file_tree_index.hpp>
#include <astra/ui/picker_index.hpp>
#include <astra/ui/workbench_layout.hpp>
#include <astra/ui/text_line_index.hpp>
#include <astra/ui/decoration_indexes.hpp>
#include <doctest/doctest.h>

#include <random>
#include <limits>
#include <set>
#include <string>

TEST_CASE("presentation text panels index mixed line endings and preserve snapshots") {
    astra::ui::TextLineIndex lines;
    lines.reset("中\r\n🙂\re\xCC\x81\n");
    REQUIRE(lines.size()==4);CHECK(lines.bytes()==14);
    CHECK(lines.at(0)=="中");CHECK(lines.at(1)=="🙂");CHECK(lines.at(2)=="e\xCC\x81");CHECK(lines.at(3).empty());
    CHECK(lines.offset(1)==5);CHECK(lines.offset(2)==10);CHECK(lines.offset(3)==14);
    const auto old=lines;
    std::string many;
    for(int row=0;row<20000;++row)many+=std::to_string(row)+"\n";
    lines.reset(many);
    std::vector<std::string> viewport;
    lines.for_each_range(18000,18004,[&](std::size_t row,const std::string& text){CHECK(row==18000+viewport.size());viewport.push_back(text);});
    CHECK(viewport==std::vector<std::string>{"18000","18001","18002","18003"});
    CHECK(old.at(0)=="中");CHECK(old.size()==4);
    CHECK_THROWS_AS(lines.reset(std::string(8*1024*1024+1,'x')),std::length_error);
    CHECK(lines.size()==20001);
    lines.reset("");CHECK(lines.size()==1);CHECK(lines.bytes()==0);CHECK(lines.at(0).empty());
}

TEST_CASE("presentation range queries prune overlaps and retain shared snapshots") {
    using Index=astra::ui::RangeIndex<std::size_t,astra::ui::Utf8ByteUnit>;
    std::vector<Index::Entry> entries;
    for(std::size_t i=0;i<99999;++i)entries.push_back({i*10,i*10+2,i});
    entries.push_back({0,1000000,100000});Index index;index.reset(entries);
    const auto retained=index;std::vector<std::size_t> found;
    const auto statistics=index.intersect(999000,999003,[&](const auto& entry){found.push_back(entry.value);});
    CHECK(found==std::vector<std::size_t>{100000,99900});CHECK(statistics.matches==2);CHECK(statistics.nodes<150);
    CHECK(index.intersect(500,500,[](const auto&){}).nodes==0);CHECK(index.validate());
    entries[50000].value=200000;index.reset(entries);
    CHECK(index.shared_prefix_items(retained)==50001);CHECK(index.shared_suffix_items(retained)==49998);
    found.clear();retained.intersect(500000,500001,[&](const auto& entry){found.push_back(entry.value);});
    CHECK(found==std::vector<std::size_t>{100000,50000});
    const auto unchanged=index;CHECK_THROWS_AS(index.reset({{2,2,0}}),std::invalid_argument);
    CHECK(index.shared_prefix_items(unchanged)==100000);
    CHECK_THROWS_AS(index.intersect(3,2,[](const auto&){}),std::invalid_argument);
    index.clear();CHECK(index.size()==0);CHECK(retained.size()==100000);
}

TEST_CASE("presentation overlapping ranges match a flat reference after random replacements") {
    using Index=astra::ui::RangeIndex<std::size_t,astra::ui::DocumentLineUnit>;
    std::mt19937 random(0xA57A1D);Index index;
    for(unsigned round=0;round<150;++round) {
        const auto retained=index;std::vector<Index::Entry> entries;
        for(std::size_t item=0;item<500;++item){const auto first=static_cast<std::size_t>(random()%5000);entries.push_back({first,first+1+random()%200,item});}
        index.reset(entries);std::stable_sort(entries.begin(),entries.end(),[](const auto& left,const auto& right){return left.begin<right.begin;});
        CHECK(index.validate());CHECK(retained.validate());
        for(unsigned query=0;query<20;++query) {
            const auto begin=static_cast<std::size_t>(random()%5200);const auto end=begin+random()%20;
            std::vector<Index::Entry> expected,actual;
            if(begin!=end)for(const auto& entry:entries)if(entry.begin<end && entry.end>begin)expected.push_back(entry);
            index.intersect(begin,end,[&](const auto& entry){actual.push_back(entry);});CHECK(actual==expected);
        }
        const auto old=index;index.reset(entries);CHECK(index.shared_prefix_items(old)==entries.size());
    }
}

TEST_CASE("presentation decoration adapters distinguish bytes lines and UTF16 columns") {
    using Json=astra::protocol::Json;astra::ui::DecorationIndexes indexes;
    indexes.set_highlights(Json::array({{{"begin",3},{"end",7},{"kind","string"}}}));
    indexes.set_diagnostics(Json::array({{{"range",{{"start",{{"line",2},{"character",4}}},{"end",{{"line",4},{"character",6}}}}},{"severity",2}}}));
    indexes.set_gutter(Json::array({{{"line",4},{"count",0},{"kind","deleted"}},{{"line",8},{"count",3},{"kind","added"}}}));
    std::size_t matches{};indexes.highlights.intersect(4,5,[&](const auto& entry){++matches;CHECK(entry.begin==3);CHECK(entry.end==7);CHECK(entry.value=="string");});CHECK(matches==1);
    matches=0;indexes.diagnostics.intersect(3,4,[&](const auto& entry){++matches;CHECK(entry.begin==2);CHECK(entry.end==5);CHECK(entry.value.start_utf16==4);CHECK(entry.value.end_utf16==6);CHECK(entry.value.severity==2);});CHECK(matches==1);
    CHECK(indexes.diagnostics.intersect(5,6,[](const auto&){}).matches==0);
    CHECK(indexes.gutter.intersect(4,5,[](const auto&){}).matches==1);CHECK(indexes.gutter.intersect(11,12,[](const auto&){}).matches==0);
    const auto retained=indexes;
    CHECK_THROWS_AS(indexes.set_highlights(Json::array({{{"begin",-1},{"end",3},{"kind","type"}}})),std::invalid_argument);
    CHECK_THROWS_AS(indexes.set_gutter(Json::array({{{"line",std::numeric_limits<std::size_t>::max()},{"count",1},{"kind","added"}}})),std::length_error);
    CHECK(indexes.highlights.shared_prefix_items(retained.highlights)==1);CHECK(indexes.gutter.shared_prefix_items(retained.gutter)==2);
    indexes.set_diagnostics(Json::array());CHECK(indexes.diagnostics.size()==0);CHECK(retained.diagnostics.size()==1);
}

TEST_CASE("presentation confirmation layout shares bounded review and button regions") {
    for(const auto scale:{1.0F,1.25F,1.5F,2.0F})for(const auto width:{260.0F,640.0F,1440.0F})for(const std::size_t rows:{0,1,5,40}){
        const auto layout=astra::ui::ConfirmationLayout::measure(width*scale,600*scale,scale,rows);
        CHECK(layout.panel.x>=0);CHECK(layout.panel.right()<=width*scale);CHECK(layout.panel.bottom()<=600*scale);
        CHECK(layout.accept.right()<=layout.cancel.x);CHECK(layout.items.bottom()<=layout.accept.y);
        CHECK(layout.accept.contains(layout.accept.x+layout.accept.width/2,layout.accept.y+layout.accept.height/2));
        CHECK((layout.items.height>0)==(rows>0));
        if(rows==1)CHECK(layout.items.height<=28*scale+1);
        if(rows==40)CHECK(layout.items.height<40*28*scale);
    }
}

TEST_CASE("presentation choice buttons remain distinct and inside narrow and scaled dialogs") {
    for(const auto scale:{1.0F,1.25F,1.5F,2.0F})for(const auto width:{40.0F,260.0F,640.0F,1440.0F})for(const auto height:{40.0F,200.0F,600.0F})for(std::size_t choices=2;choices<=4;++choices) {
        const auto layout=astra::ui::ChoiceLayout::measure(width*scale,height*scale,scale,choices);
        REQUIRE(layout.buttons.size()==choices);
        CHECK(layout.panel.x>=0);CHECK(layout.panel.y>=0);CHECK(layout.panel.right()<=width*scale);CHECK(layout.panel.bottom()<=height*scale);
        CHECK(layout.title.bottom()<=layout.detail.y);CHECK(layout.detail.bottom()<=layout.buttons.front().y);
        for(std::size_t i=0;i<choices;++i) {
            const auto& box=layout.buttons[i];
            CHECK(box.width>0);CHECK(box.height>0);CHECK(box.x>=layout.panel.x);CHECK(box.y>=layout.panel.y);
            CHECK(box.right()<=layout.panel.right());CHECK(box.bottom()<=layout.panel.bottom());
            const auto cx=box.x+box.width/2,cy=box.y+box.height/2;
            CHECK(box.contains(cx,cy));
            for(std::size_t j=0;j<choices;++j)if(i!=j)CHECK_FALSE(layout.buttons[j].contains(cx,cy));
        }
    }
    CHECK_THROWS_AS((void)astra::ui::ChoiceLayout::measure(100,100,1,1),std::invalid_argument);
    CHECK_THROWS_AS((void)astra::ui::ChoiceLayout::measure(100,100,1,5),std::invalid_argument);
}

namespace {
using Json = astra::protocol::Json;
Json entry(std::string path, bool directory) { return {{"path", path}, {"directory", directory}}; }
std::vector<std::string> flattened(const Json& source, const std::set<std::string>& collapsed) {
    std::vector<std::string> result;
    for (const auto& item : source) {
        const auto path = item.at("path").get<std::string>();
        bool hidden{};
        for (const auto& parent : collapsed) { hidden |= path.size()>parent.size() && path.starts_with(parent) && path[parent.size()]=='/'; }
        if (!hidden) { result.push_back(path); }
    }
    return result;
}
std::vector<std::string> paths(const astra::ui::FileTreeIndex::Snapshot& snapshot) {
    std::vector<std::string> result;
    snapshot.for_each([&](const auto& row) { result.push_back(row.value->at("path").template get<std::string>()); });
    return result;
}
}

TEST_CASE("presentation file tree collapse preserves nested state and old snapshots") {
    astra::ui::FileTreeIndex index;
    const auto source = Json::array({entry("a",true), entry("a/b",true), entry("a/b/x.cpp",false), entry("a/y.js",false), entry("z.md",false)});
    index.reset(source);
    const auto snapshot = index.snapshot();
    index.toggle(1); CHECK(paths(index.snapshot()) == std::vector<std::string>{"a","a/b","a/y.js","z.md"});
    index.toggle(0); CHECK(paths(index.snapshot()) == std::vector<std::string>{"a","z.md"});
    index.toggle(0); CHECK(paths(index.snapshot()) == std::vector<std::string>{"a","a/b","a/y.js","z.md"});
    index.toggle(1); CHECK(paths(index.snapshot()) == flattened(source, {}));
    CHECK(paths(snapshot) == flattened(source, {})); CHECK(index.snapshot().validate());
    CHECK_THROWS_AS(index.toggle(4), std::invalid_argument);
    index.toggle(0); index.reset(source); CHECK(index.size()==2);
    index.reset(Json::array({entry("z.md",false)})); CHECK(index.size()==1); CHECK_FALSE(index.collapsed("a"));
    CHECK_THROWS_AS(index.reset(Json::array({entry("z",false),entry("z",false)})),std::invalid_argument);
    CHECK(index.at(0)["path"]=="z.md");
}

TEST_CASE("presentation picker filtering preserves source identity and old snapshots") {
    astra::ui::PickerIndex index;
    std::vector<std::string> labels;
    for(std::size_t i=0;i<2000;++i)labels.push_back("File "+std::to_string(i)+" 中🙂");
    index.reset(labels);
    const auto retained=index.snapshot();
    std::mt19937 random(0xA57A91);
    for(int iteration=0;iteration<150;++iteration){
        const auto query=iteration%3==0 ? std::string("中🙂") : "fILE "+std::to_string(random()%2000);
        index.filter(query);
        std::vector<std::size_t> expected;
        auto normalized=query;
        for(auto& byte:normalized)if(byte>='A'&&byte<='Z')byte=static_cast<char>(byte-'A'+'a');
        for(std::size_t i=0;i<labels.size();++i){
            auto folded=labels[i];folded[0]='f';
            if(folded.find(normalized)!=folded.npos)expected.push_back(i);
        }
        REQUIRE(index.size()==expected.size());
        index.for_each_range(0,index.size(),[&](std::size_t row,const auto& item){CHECK(item.source==expected[row]);CHECK(item.label==labels[item.source]);});
        CHECK(index.snapshot().validate());CHECK(retained.size()==labels.size());CHECK(retained.at(1999).label==labels.back());
    }
    index.filter("");CHECK(index.size()==2000);
    CHECK_THROWS_AS(index.reset({"invalid\nlabel"}),std::length_error);CHECK(index.size()==2000);
    CHECK_THROWS_AS(index.reset(std::vector<std::string>(4097,"row")),std::length_error);
    CHECK_THROWS_AS(index.reset({std::string(4097,'x')}),std::length_error);
    CHECK_THROWS_AS(index.reset(std::vector<std::string>(17,std::string(4096,'x'))),std::length_error);
}

TEST_CASE("presentation modal layouts share bounded rows and pointer geometry") {
    for(const auto scale:{1.0F,1.25F,1.5F,2.0F})for(const auto width:{260.0F,640.0F,1440.0F}){
        const auto picker=astra::ui::PickerLayout::measure(width*scale,600*scale,scale);
        CHECK(picker.panel.x>=0);CHECK(picker.panel.right()<=width*scale);
        CHECK(picker.input.right()<=picker.panel.right());CHECK(picker.rows.bottom()<=picker.panel.bottom());
        CHECK(picker.capacity*picker.row_height<=picker.rows.height);
        CHECK(picker.rows.contains(picker.rows.x+1,picker.rows.y+1));
        const auto text=astra::ui::TextPanelLayout::measure(width*scale,600*scale,scale);
        CHECK(text.panel.x>=0);CHECK(text.panel.right()<=width*scale);CHECK(text.content.bottom()<=text.panel.bottom());
    }
    CHECK_THROWS_AS((void)astra::ui::PickerLayout::measure(0,100,1),std::invalid_argument);
    CHECK_THROWS_AS((void)astra::ui::TextPanelLayout::measure(100,100,0),std::invalid_argument);
    CHECK(astra::ui::PickerLayout::measure(1440,900,1,1).capacity==1);
    CHECK(astra::ui::PickerLayout::measure(1440,900,1,1).panel.height<150);
    CHECK(astra::ui::TextPanelLayout::measure(1440,900,1,6).panel.height<200);
}

TEST_CASE("presentation file tree random collapse matches a flat reference") {
    constexpr std::uint32_t seed = 0xA57A2026;
    INFO(seed);
    std::mt19937 random(seed);
    Json source = Json::array();
    for (int folder=0;folder<80;++folder) {
        const auto path = "folder"+std::to_string(folder);
        source.push_back(entry(path,true));
        for (int sub=0;sub<8;++sub) {
            const auto child=path+"/sub"+std::to_string(sub);
            source.push_back(entry(child,true));
            for (int file=0;file<4;++file) { source.push_back(entry(child+"/file"+std::to_string(file)+".cpp",false)); }
        }
    }
    astra::ui::FileTreeIndex index; index.reset(source);
    std::set<std::string> collapsed;
    for (int iteration=0;iteration<300;++iteration) {
        const auto expected = flattened(source,collapsed);
        const auto retained=index.snapshot();
        const auto position = random()%index.size();
        if (index.at(position)["directory"].get<bool>()) {
            const auto path = index.at(position)["path"].get<std::string>();
            if (!collapsed.erase(path)) { collapsed.insert(path); }
            index.toggle(position);
        }
        CHECK(paths(index.snapshot())==flattened(source,collapsed));
        CHECK(paths(retained)==expected); CHECK(index.snapshot().validate());
        std::size_t visited{};
        const auto count=std::min(index.size(),std::size_t{20});
        index.for_each_range(index.size()-count,index.size(),[&](std::size_t row,const Json& value){CHECK(value==index.at(row));++visited;});
        CHECK(visited==count);
    }
}

TEST_CASE("presentation measured prefix seeking uses ordered nonadditive summaries") {
    using Index=astra::ui::FileTreeIndex;
    std::vector<Index::Row> rows;
    for (std::size_t i=0;i<100;++i) { rows.push_back({std::make_shared<const Json>(entry(std::to_string(i),false)),i*3,i*3+1}); }
    Index::Snapshot tree(std::move(rows));
    for (std::size_t target=0;target<310;++target) {
        const auto result=tree.seek_prefix([&](const Index::Summary& value){return value.last_source>target;});
        const auto expected=std::min((target+2)/3,std::size_t{100});
        CHECK(result.index==expected);
        if (expected<100) { REQUIRE(result.item); CHECK(result.item->ordinal==expected*3); }
        else { CHECK(result.item==nullptr); }
    }
}

TEST_CASE("presentation workbench layout stays bounded across DPI and narrow windows") {
    for (const auto scale : {1.0F,1.25F,1.5F,2.0F}) {
        for (const auto width : {260.0F,640.0F,1440.0F}) {
            const auto layout=astra::ui::WorkbenchLayout::measure(width*scale,900*scale,scale,true,300,true,true);
            CHECK(layout.editor.x==layout.sidebar.right());
            CHECK(layout.editor.right()==doctest::Approx(width*scale));
            CHECK(layout.editor.bottom()==layout.status.y);
            CHECK(layout.sidebar_rows.bottom()==layout.outline_header.y);
            CHECK(layout.outline_rows.bottom()==layout.status.y);
            CHECK(layout.editor.width>=0); CHECK(layout.sidebar.width>=0);
            CHECK(layout.sidebar_handle.contains(layout.editor.x,100*scale)==(layout.sidebar.width>0));
        }
    }
    const auto closed=astra::ui::WorkbenchLayout::measure(1440,900,1,false,300,false,true);
    CHECK(closed.editor.x==44); CHECK(closed.sidebar.width==0);
    CHECK_THROWS_AS((void)astra::ui::WorkbenchLayout::measure(0,900,1,true,300,true,true),std::invalid_argument);
}

TEST_CASE("presentation tabs use measured labels reveal active tabs and expose close hits") {
    using astra::ui::TabStrip;
    auto strip=TabStrip::measure({44,32,600,34},1,{40,150,20},0,0,true);
    REQUIRE(strip.visible.size()==3); CHECK(strip.visible[0].bounds.width!=strip.visible[1].bounds.width);
    CHECK(strip.next.width==0); CHECK(strip.visible[1].close.contains(strip.visible[1].bounds.right()-10,40));
    strip=TabStrip::measure({44,32,360,34},1,{100,100,100,100},0,3,true);
    REQUIRE_FALSE(strip.visible.empty()); CHECK(strip.visible.back().index==3);
    CHECK(strip.next.right()==404); CHECK(strip.previous.width>0);
    for (const auto& tab:strip.visible) { CHECK(tab.bounds.right()<=strip.previous.x); }
}
