#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <format>
#include <meta>
#include <string_view>
#include <type_traits>

namespace tiny_orm {

    /**
     * A string that can be carried through constant evaluation: as a non-type
     * template argument, as an annotation payload, or as an element of an array
     * built by `std::define_static_array`.
     *
     * ## Why this type exists at all
     *
     * Neither `const char*` nor `std::string_view` survives that journey, for two
     * unrelated reasons. Both have to be cleared, and mixing them up accounts for
     * most of the confusing diagnostics in this area:
     *
     *  - **The type must be structural.** A class is structural only when every
     *    base and non-static data member is `public` and non-`mutable`.
     *    `std::string_view` keeps `_M_str` and `_M_len` private, so it is out --
     *    and so is `std::span`. This is about access control, not layout: this
     *    type has the same two members and the same 16 bytes as `std::string_view`,
     *    and differs only in that you can see them. That is also why the members
     *    are `data`/`size` rather than `size()` -- a data member and a member
     *    function cannot share a name, and the member is the part the language
     *    requires.
     *
     *  - **The value must be a permitted constant.** A pointer used as a constant
     *    must point at an object the language will name; a string literal is not
     *    one, since implementations may merge or duplicate literals freely. The
     *    constructor routes every string through `std::define_static_string`,
     *    which copies the characters into real static storage and hands back a
     *    nameable pointer.
     *
     * See `docs/findings-01-compile-time-strings.md` for the full reasoning and
     * the diagnostics each failure produces.
     *
     * ## Usage
     *
     * ~~~{.cpp}
     * template<compiletime_string Name> struct Table { ... };
     *
     * Table<"users">                      // literal, straight in
     * Table<snake("fullName")>            // computed -- same type as Table<"full_name">
     * struct [[=compiletime_string{"users"}]] User { ... };
     * ~~~
     */
    struct compiletime_string {
        /// Public by necessity, not by preference: see the structural rule above.
        const char* data = nullptr;
        std::size_t size = 0;

        constexpr compiletime_string() = default;

        /**
         * Builds from anything string-ish: a literal, a `const char*`, a
         * `std::string_view`, or a `std::string` computed during constant
         * evaluation.
         *
         * The parameter is deliberately `const S&` rather than
         * `std::string_view`. With a `std::string_view` parameter, writing
         * `Table<"users">` would need `const char[6]` -> `std::string_view` ->
         * `compiletime_string`, which is **two** user-defined conversions, and
         * the language permits only one:
         *
         *     error: could not convert '"users"' from 'const char [6]'
         *            to 'compiletime_string'
         *
         * Accepting the source type directly collapses that to one conversion,
         * which is what makes both `Table<"users">` and a bare `return some_string;`
         * compile. The conversion is implicit on purpose, for exactly that reason.
         *
         * `consteval` rather than `constexpr` because `std::define_static_string`
         * is itself `consteval`: construction can only ever happen at compile time,
         * and saying so moves the error to the call site instead of surfacing it
         * from inside this constructor. Copying and reading a value at run time is
         * unaffected -- only construction is restricted.
         */
        template<typename S>
            requires std::convertible_to<const S&, std::string_view>
        consteval compiletime_string(const S& s) { // NOLINT: implicit on purpose
            auto const sv = std::string_view{s};
            data = std::define_static_string(sv);
            size = sv.size();
        }

        [[nodiscard]] constexpr auto view() const -> std::string_view { return {data, size}; }
        [[nodiscard]] constexpr auto begin() const -> const char* { return data; }
        [[nodiscard]] constexpr auto end() const -> const char* { return data + size; }
        [[nodiscard]] constexpr bool empty() const { return size == 0; }

        /**
         * Member-wise equality, which is also how the compiler decides whether
         * `Table<a>` and `Table<b>` name the same type. Because
         * `std::define_static_string` is content-addressed -- equal text always
         * yields the same pointer -- comparing pointer and size agrees with
         * comparing the text. That coherence between `==` and template-argument
         * identity is exactly what `std::string_view` lacks, and the reason it was
         * excluded from being structural in the first place.
         */
        friend constexpr bool operator==(compiletime_string, compiletime_string) = default;

        /**
         * Deliberately **not** defaulted. A defaulted `<=>` would order by the
         * address of the promoted storage, which is arbitrary and may differ
         * between builds. Delegating to `view()` gives real lexicographic order,
         * so this type is safe as a `std::map` key or in a sorted column list.
         */
        friend constexpr auto operator<=>(compiletime_string a, compiletime_string b) {
            return a.view() <=> b.view();
        }
    };

    /// Enforces the invariant the whole type exists to satisfy.
    static_assert(std::is_structural_v<compiletime_string>);

} // namespace tiny_orm

/**
 * Lets a `compiletime_string` drop straight into `std::format` while generating
 * SQL -- `std::format("SELECT {} FROM {}", col, tbl)` -- with no `.view()` at
 * every use site.
 */
template<>
struct std::formatter<tiny_orm::compiletime_string> : std::formatter<std::string_view> {
    auto format(tiny_orm::compiletime_string s, auto& ctx) const {
        return std::formatter<std::string_view>::format(s.view(), ctx);
    }
};
