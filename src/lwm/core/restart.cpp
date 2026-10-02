#include "restart.hpp"
#include "invariants.hpp"
#include <cstring>
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

Fixture const* Snapshot::find_fixture(xcb_window_t window) const
{
    auto fixture = std::ranges::find(fixtures, window, &Fixture::id);
    return fixture == fixtures.end() ? nullptr : &*fixture;
}

std::vector<uint32_t> encode(Snapshot const& snapshot)
{
    // X11 output names are byte strings. Preserve them even if they are not UTF-8.
    auto payload = rfl::json::write<Wire>(snapshot, YYJSON_WRITE_ALLOW_INVALID_UNICODE);
    if (payload.size() > UINT32_MAX)
        throw std::length_error("Restart snapshot is too large");
    std::vector<uint32_t> words(2 + (payload.size() + 3) / 4);
    words[0] = format;
    words[1] = static_cast<uint32_t>(payload.size());
    std::memcpy(words.data() + 2, payload.data(), payload.size());
    return words;
}

std::optional<Snapshot> decode(std::span<uint32_t const> words)
{
    if (words.size() < 2 || words[0] != format || words.size() != 2 + (size_t{ words[1] } + 3) / 4)
        return std::nullopt;
    auto bytes = std::string_view(reinterpret_cast<char const*>(words.data() + 2), (words.size() - 2) * 4);
    if (bytes.substr(words[1]).find_first_not_of('\0') != bytes.npos)
        return std::nullopt;
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(
        yyjson_read(bytes.data(), words[1], YYJSON_READ_ALLOW_INVALID_UNICODE),
        &yyjson_doc_free
    );
    if (!document)
        return std::nullopt;
    auto snapshot = rfl::parsing::Parser<Reader, rfl::json::Writer, Snapshot, Wire>::read(
        Reader{},
        Reader::InputVarType(yyjson_doc_get_root(document.get()))
    );
    if (!snapshot || invariants::validate(*snapshot).has_value())
        return std::nullopt;
    return std::move(*snapshot);
}

} // namespace lwm::restart
