#include <astra/ui/file_tree_index.hpp>
#include <astra/ui/picker_index.hpp>
#include <astra/ui/workbench_layout.hpp>
#include <astra/ui/text_line_index.hpp>
#include <doctest/doctest.h>

#include <random>
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

TEST_CASE("presentation confirmation layout shares bounded review and button regions") {
    for(const auto scale:{1.0F,1.25F,1.5F,2.0F})for(const auto width:{260.0F,640.0F,1440.0F})for(const auto review:{false,true}){
        const auto layout=astra::ui::ConfirmationLayout::measure(width*scale,600*scale,scale,review);
        CHECK(layout.panel.x>=0);CHECK(layout.panel.right()<=width*scale);CHECK(layout.panel.bottom()<=600*scale);
        CHECK(layout.accept.right()<=layout.cancel.x);CHECK(layout.items.bottom()<=layout.accept.y);
        CHECK(layout.accept.contains(layout.accept.x+layout.accept.width/2,layout.accept.y+layout.accept.height/2));
        CHECK((layout.items.height>0)==review);
    }
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
