#include <astra/ui/inline_form.hpp>
#include <astra/core/utf8.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace astra::ui {
namespace {
using Json = protocol::Json;
void text_valid(std::string_view text, std::size_t limit) {
    if (text.size() > limit || text.find_first_of("\r\n") != text.npos || text.find('\0') != text.npos) {
        throw protocol::RpcError(-32602, "Inline form text exceeds its single-line bound");
    }
    utf8::validate(text);
}
std::string text(const Json& spec, const char* key, std::size_t limit, std::string fallback = {}) {
    const auto value = spec.value(key, std::move(fallback)); text_valid(value, limit); return value;
}
} // namespace

InlineForm::InlineForm(const Json& spec,std::string epoch) : epoch_(std::move(epoch)) {
    id_ = text(spec,"id",128); title_ = text(spec,"title",128);
    if (id_.empty() || title_.empty()) { throw protocol::RpcError(-32602,"Inline form identity and title required"); }
    const auto fields = spec.at("fields"), toggles = spec.value("toggles",Json::array()), actions = spec.at("actions");
    if (!fields.is_array() || fields.empty() || fields.size()>4 || !toggles.is_array() || toggles.size()>4 ||
        !actions.is_array() || actions.empty() || actions.size()>4) {
        throw protocol::RpcError(-32602,"Inline form allows 1..4 inputs/actions and 0..4 toggles");
    }
    std::set<std::string> ids, action_ids, shortcuts;
    for (const auto& item : actions) {
        const auto action = text(item,"id",128), label = text(item,"label",64);
        if (action.empty() || label.empty() || !ids.insert(action).second) { throw protocol::RpcError(-32602,"Invalid inline action identity"); }
        actions_.push_back({action,label}); action_ids.insert(action);
    }
    for (const auto& item : fields) {
        auto field = text(item,"id",128), value = text(item,"value",max_input_bytes);
        const auto submit = text(item,"submit",128), shifted = text(item,"shiftSubmit",128);
        if (field.empty() || !ids.insert(field).second || (!submit.empty() && !action_ids.contains(submit)) ||
            (!shifted.empty() && !action_ids.contains(shifted))) { throw protocol::RpcError(-32602,"Invalid inline input/action identity"); }
        fields_.push_back({std::move(field),text(item,"placeholder",256),submit,shifted,astra::Document(BufferId{fields_.size()+1},value,32)});
    }
    for (const auto& item : toggles) {
        const auto field = text(item,"id",128), label = text(item,"label",32), shortcut = text(item,"shortcut",32);
        if (field.empty() || label.empty() || !ids.insert(field).second || (!shortcut.empty() && !shortcuts.insert(shortcut).second)) {
            throw protocol::RpcError(-32602,"Invalid inline toggle identity or shortcut");
        }
        if(!shortcut.empty() && (shortcut.size()!=5 || !shortcut.starts_with("Alt+") || shortcut[4]<'A' || shortcut[4]>'Z')) {
            throw protocol::RpcError(-32602,"Inline toggle shortcuts must be Alt plus an uppercase letter");
        }
        toggles_.push_back({field,label,shortcut,item.value("value",false)});
    }
    const auto requested = text(spec,"focus",128,fields_.front().id);
    const auto selected = std::find_if(fields_.begin(),fields_.end(),[&](const auto& field){return field.id==requested;});
    if (selected==fields_.end()) { throw protocol::RpcError(-32602,"Unknown inline input focus"); }
    focus(static_cast<std::size_t>(selected-fields_.begin()),true);
}
std::string InlineForm::version() const { return epoch_.empty() ? std::to_string(version_) : epoch_+"-"+std::to_string(version_); }
InlineForm::Json InlineForm::state() const {
    auto values = Json::object();
    for (const auto& field : fields_) values[field.id] = field.text.text().str();
    for (const auto& toggle : toggles_) values[toggle.id] = toggle.value;
    return {{"id",id_},{"version",version()},{"values",std::move(values)}};
}
void InlineForm::require_generation() const {
    if (version_==std::numeric_limits<std::uint64_t>::max()) { throw protocol::RpcError(-32004,"Inline input generation exhausted"); }
}
void InlineForm::changed() {
    ++version_; busy_=false;error_=false;message_.clear();
}
void InlineForm::focus(std::optional<std::size_t> index, bool select_all) {
    if (index && *index>=fields_.size()+toggles_.size()+actions_.size()) { throw protocol::RpcError(-32602,"Inline focus is out of range"); }
    focus_=index;
    if (index && *index<fields_.size() && select_all) { static_cast<void>(editing::execute(fields_[*index].text,editing::Command::select_all)); }
}
void InlineForm::cycle_focus(bool backwards) {
    const auto count=fields_.size()+toggles_.size()+actions_.size();
    focus_=focus_ ? (*focus_+(backwards ? count-1 : 1))%count : 0;
}
bool InlineForm::edit(editing::Command command, const editing::Options& options) {
    if (!focus_ || *focus_>=fields_.size()) return false;
    auto& document=fields_[*focus_].text;
    using editing::Command;
    switch(command) {
    case Command::move_left: case Command::move_right: case Command::move_word_left: case Command::move_word_right:
    case Command::move_home: case Command::move_end: case Command::select_all: case Command::backspace:
    case Command::delete_forward: case Command::delete_word_backward: case Command::delete_word_forward:
    case Command::insert_text: break;
    default: throw protocol::RpcError(-32602,"Unsupported single-line input command");
    }
    if (command==editing::Command::insert_text) {
        text_valid(options.text,max_input_bytes);
        const auto& selection=document.selections().front();
        if (options.text.size()>max_input_bytes-(document.text().size_bytes()-(selection.end().value-selection.begin().value))) {
            throw protocol::RpcError(-32602,"Inline input exceeds 16 KiB");
        }
    }
    const auto before=document.content_identity();
    require_generation();
    static_cast<void>(editing::execute(document,command,options));
    if (before==document.content_identity()) return false;
    changed();return true;
}
bool InlineForm::history(bool redo) {
    if (!focus_ || *focus_>=fields_.size()) return false;
    require_generation();
    const bool modified=redo ? fields_[*focus_].text.redo() : fields_[*focus_].text.undo();
    if (modified) changed();return modified;
}
void InlineForm::select(std::size_t field, ByteOffset offset, bool extend) {
    auto& document=fields_.at(field).text;
    document.set_selections({{extend ? document.selections().front().anchor : offset,offset}});
    focus(field);
}
void InlineForm::toggle(std::size_t index) {
    auto& value=toggles_.at(index).value;require_generation();value=!value;changed();
}
bool InlineForm::update(const Json& params) {
    if (params.at("id")!=id_ || params.at("version")!=version()) return false;
    const auto message=text(params,"message",512,message_);
    const auto busy=params.value("busy",busy_);
    const auto error=params.value("error",error_);
    message_=message;busy_=busy;error_=error;return true;
}

InlineFormLayout InlineFormLayout::measure(Box editor,float scale,std::size_t fields,std::size_t toggles,std::size_t actions) {
    if (!std::isfinite(scale) || scale<=0 || fields<1 || fields>4 || toggles>4 || actions<1 || actions>4 ||
        !std::isfinite(editor.x) || !std::isfinite(editor.y) || !std::isfinite(editor.width) || !std::isfinite(editor.height) || editor.width<0 || editor.height<0) {
        throw std::invalid_argument("Invalid inline form geometry");
    }
    InlineFormLayout result;
    const auto inset=std::min(8*scale,std::min(editor.width,editor.height)/8);
    const auto width=std::max(0.0F,std::min(490*scale,editor.width-2*inset));
    const auto requested=(62+34*static_cast<float>(fields)+(toggles ? 26 : 0))*scale;
    const auto height=std::max(0.0F,std::min(requested,editor.height-2*inset));
    result.panel={editor.right()-inset-width,editor.y+inset,width,height};
    const auto sx=width>0 ? width/(490*scale) : 0;
    const auto sy=requested>0 ? height/requested : 0;
    const auto box=[&](float x,float y,float w,float h){return Box{result.panel.x+x*scale*sx,result.panel.y+y*scale*sy,w*scale*sx,h*scale*sy};};
    result.title=box(10,5,438,20);result.close=box(458,2,28,24);
    for(std::size_t i=0;i<fields;++i)result.fields.push_back(box(8,28+34*static_cast<float>(i),474,28));
    const auto bottom=28+34*static_cast<float>(fields);
    if(toggles)for(std::size_t i=0;i<toggles;++i)result.toggles.push_back(box(8+static_cast<float>(i)*84,bottom,78,22));
    const auto action_y=bottom+(toggles ? 26 : 0);
    result.message=box(10,action_y+4,218,20);
    const auto action_width=246/static_cast<float>(actions);
    for(std::size_t i=0;i<actions;++i)result.actions.push_back(box(236+static_cast<float>(i)*action_width,action_y,action_width-4,26));
    return result;
}
} // namespace astra::ui
