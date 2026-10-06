#include "restart.hpp"
#include "invariants.hpp"
#include <memory>
#include <rfl/AddTagsToVariants.hpp>
#include <rfl/NoExtraFields.hpp>
#include <rfl/NoOptionals.hpp>
#include <rfl/json/write.hpp>
#include <unordered_set>
#include <utility>

namespace lwm::restart {
namespace {

// Tighten the library reader at the handoff boundary: no ambiguous objects
// or variant tags, and no integer narrowing (including signed X11 coordinates).
struct Reader : rfl::json::Reader
{
    rfl::Result<InputObjectType> to_object(InputVarType value) const noexcept
    {
        auto object = rfl::json::Reader::to_object(value);
        if (!object)
            return object;
        std::unordered_set<std::string_view> names;
        size_t index, count;
        yyjson_val *key, *field;
        yyjson_obj_foreach(value.val_, index, count, key, field)
        {
            if (!names.emplace(yyjson_get_str(key), yyjson_get_len(key)).second)
                return rfl::error("duplicate restart field");
        }
        return object;
    }

    using rfl::json::Reader::read_object;
    template <typename Processors, typename... Fields>
    std::optional<rfl::Error> read_object(
        rfl::parsing::FieldVariantReader<Reader, rfl::json::Writer, Processors, Fields...> const& reader,
        InputObjectType object
    ) const noexcept
    {
        if (yyjson_obj_size(object.val_) != 1)
            return rfl::Error("restart mode must have exactly one tag");
        return rfl::json::Reader::read_object(reader, object);
    }

    template <typename T> rfl::Result<T> to_basic_type(InputVarType value) const noexcept
    {
        if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
        {
            if (yyjson_is_uint(value.val_) && std::in_range<T>(yyjson_get_uint(value.val_)))
                return static_cast<T>(yyjson_get_uint(value.val_));
            if (yyjson_is_sint(value.val_) && std::in_range<T>(yyjson_get_sint(value.val_)))
                return static_cast<T>(yyjson_get_sint(value.val_));
            return rfl::error("restart integer is out of range or has the wrong type");
        }
        else
            return rfl::json::Reader::to_basic_type<T>(value);
    }
};

using Wire = rfl::Processors<rfl::NoExtraFields, rfl::NoOptionals, rfl::AddTagsToVariants>;

} // namespace

ClientIntent const* Snapshot::find(xcb_window_t window) const
{
    auto record = std::ranges::find(clients, window, &ClientIntent::id);
    return record == clients.end() ? nullptr : &*record;
}

FixtureIntent const* Snapshot::find_fixture(xcb_window_t window) const
{
    auto fixture = std::ranges::find(fixtures, window, &FixtureIntent::id);
    return fixture == fixtures.end() ? nullptr : &*fixture;
}

std::string encode(Snapshot const& snapshot)
{
    return rfl::json::write<Wire>(snapshot, YYJSON_WRITE_ALLOW_INVALID_UNICODE);
}

std::optional<Snapshot> decode(std::string_view text)
{
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(
        yyjson_read(text.data(), text.size(), YYJSON_READ_ALLOW_INVALID_UNICODE),
        &yyjson_doc_free
    );
    if (!document)
        return std::nullopt;
    auto snapshot = rfl::parsing::Parser<Reader, rfl::json::Writer, Snapshot, Wire>::read(
        Reader{},
        Reader::InputVarType(yyjson_doc_get_root(document.get()))
    );
    if (!snapshot || snapshot->format != format || invariants::validate(*snapshot).has_value())
        return std::nullopt;
    return std::move(*snapshot);
}

} // namespace lwm::restart
