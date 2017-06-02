// Copyright (C) 2016-2017 Jonathan Müller <jonathanmueller.dev@gmail.com>
// This file is subject to the license terms in the LICENSE file
// found in the top-level directory of this distribution.

#include "cmark_ext_command.hpp"

#include <cassert>
#include <cstdint>
#include <string>

#include <type_safe/flag.hpp>
#include <type_safe/optional.hpp>

#include <cmark_extension_api.h>

using namespace standardese::comment;
using namespace standardese::comment::detail;

namespace
{
    void set_raw_command_type(cmark_node* node, unsigned cmd)
    {
        static_assert(sizeof(void*) >= sizeof(unsigned), "fix me for your platform");
        auto as_void_ptr = reinterpret_cast<void*>(cmd);
        cmark_node_set_user_data(node, as_void_ptr);
    }

    unsigned get_raw_command_type(cmark_node* node)
    {
        auto as_void_ptr = cmark_node_get_user_data(node);
        return unsigned(reinterpret_cast<std::uintptr_t>(as_void_ptr));
    }

    bool is_whitespace(char c)
    {
        return c == ' ' || c == '\t' || c == '\n';
    }

    void skip_whitespace(char*& cur)
    {
        while (*cur && is_whitespace(*cur))
            ++cur;
    }

    std::string parse_word(char*& cur)
    {
        skip_whitespace(cur);

        std::string word;
        for (; *cur && !is_whitespace(*cur); ++cur)
            word += *cur;

        return word;
    }

    unsigned try_parse_command(char*& cur, const config& c)
    {
        if (*cur == c.command_character())
        {
            ++cur;

            auto command = parse_word(cur);
            return c.try_lookup(command.c_str());
        }
        else
            return unsigned(command_type::invalid);
    }

    bool accept_commands(cmark_node* parent_container)
    {
        if (cmark_node_get_type(parent_container) == node_section()
            || cmark_node_get_type(parent_container) == node_command())
            // don't allow commands in commands
            return false;
        // allow at most one parent for a command
        return cmark_node_get_type(parent_container) == CMARK_NODE_DOCUMENT
               || cmark_node_get_type(cmark_node_parent(parent_container)) == CMARK_NODE_DOCUMENT;
    }

    void set_node(cmark_syntax_extension* self, cmark_node* node, unsigned command, const char* str)
    {
        cmark_node_set_syntax_extension(node, self);
        set_raw_command_type(node, command);
        cmark_node_set_string_content(node, str);
    }

    cmark_node* create_node(cmark_syntax_extension* self, int indent, cmark_parser* parser,
                            cmark_node* parent, cmark_node_type type, unsigned command,
                            const char* str)
    {
        auto node = cmark_parser_add_child(parser, parent, type, indent);
        set_node(self, node, command, str);
        return node;
    }

    cmark_node* create_node(cmark_syntax_extension* self, cmark_node_type type, unsigned command,
                            const char* str)
    {
        auto node = cmark_node_new(type);
        set_node(self, node, command, str);
        return node;
    }

    type_safe::optional<std::string> parse_section_key(char*& cur)
    {
        auto save       = cur;
        auto first_word = parse_word(cur);
        skip_whitespace(cur);
        if (*cur == '-')
        {
            // is a key - value section
            ++cur;
            skip_whitespace(cur);

            return first_word;
        }
        else
            // don't have a key
            cur = save;

        return type_safe::nullopt;
    }

    cmark_node* try_open_block(cmark_syntax_extension* self, int indent, cmark_parser* parser,
                               cmark_node* parent_container, unsigned char* input, int len)
    {
        if (!accept_commands(parent_container))
            return nullptr;

        const auto& config =
            *static_cast<standardese::comment::config*>(cmark_syntax_extension_get_private(self));
        auto cur     = reinterpret_cast<char*>(input);
        auto command = try_parse_command(cur, config);
        if (is_section(command))
        {
            auto node =
                create_node(self, indent, parser, parent_container, node_section(), command,
                            parse_section_key(cur).map(&std::string::c_str).value_or(nullptr));

            // skip command
            cmark_parser_advance_offset(parser, reinterpret_cast<char*>(input),
                                        int(cur - reinterpret_cast<char*>(input)), 0);

            return node;
        }
        else if (is_command(command))
        {
            // skip rest of line
            skip_whitespace(cur);
            cmark_parser_advance_offset(parser, reinterpret_cast<char*>(input), len, 0);

            return create_node(self, indent, parser, parent_container, node_command(), command,
                               cur); // store remainder of line as arguments
        }
        else
            return nullptr;
    }

    bool paragraph_can_be_brief(cmark_node* paragraph)
    {
        assert(cmark_node_get_type(paragraph) == CMARK_NODE_PARAGRAPH);

        for (auto child = cmark_node_first_child(paragraph); child; child = cmark_node_next(child))
            if (cmark_node_get_type(child) == CMARK_NODE_SOFTBREAK
                || cmark_node_get_type(child) == CMARK_NODE_LINEBREAK)
                // multi-line paragraph, can't be
                return false;

        return true;
    }

    cmark_node* prev_details(cmark_node* cur)
    {
        // loop back, skipping over commands in the process
        auto details = cmark_node_previous(cur);
        while (details && cmark_node_get_type(details) == node_command())
            details = cmark_node_previous(details);

        return details && cmark_node_get_type(details) == node_section()
                       && detail::get_section_type(details) == section_type::details ?
                   details :
                   nullptr;
    }

    cmark_node* wrap_in_details(cmark_syntax_extension* self, cmark_node* cur)
    {
        auto details = prev_details(cur);
        if (!details)
        {
            // create new detail section
            details = create_node(self, node_section(), unsigned(section_type::details), nullptr);
            cmark_node_replace(cur, details);
        }

        cmark_node_append_child(details, cur);
        return details;
    }

    // post process function
    cmark_node* create_implicit_brief_details(cmark_syntax_extension* self, cmark_parser*,
                                              cmark_node*             root)
    {
        type_safe::flag need_brief(true);
        for (auto cur = cmark_node_first_child(root); cur; cur = cmark_node_next(cur))
        {
            if (need_brief.try_reset() && cmark_node_get_type(cur) == CMARK_NODE_PARAGRAPH)
            {
                if (paragraph_can_be_brief(cur))
                {
                    // create implicit brief
                    auto node =
                        create_node(self, node_section(), unsigned(section_type::brief), nullptr);

                    cmark_node_replace(cur, node);
                    cmark_node_append_child(node, cur);
                    cur = node;
                }
                else
                    cur = wrap_in_details(self, cur);
            }
            else if (cmark_node_get_type(cur) == node_section()
                     && detail::get_section_type(cur) == section_type::brief)
            {
                need_brief.reset(); // don't need brief anymore
            }
            else if (cmark_node_get_type(cur) != node_section()
                     && cmark_node_get_type(cur) != node_command())
            {
                need_brief.reset(); // no implicit brief possible anymore
                cur = wrap_in_details(self, cur);
            }
        }

        // don't have a new root node
        return nullptr;
    }
}

cmark_syntax_extension* standardese::comment::detail::create_command_extension(config& c)
{
    auto ext = cmark_syntax_extension_new("standardese_command");

    cmark_syntax_extension_set_get_type_string_func(ext, [](cmark_syntax_extension*,
                                                            cmark_node* node) {
        if (cmark_node_get_type(node) == node_command())
            return "standardese_command";
        else if (cmark_node_get_type(node) == node_section())
            return "standardese_section";
        else
            return "<unknown>";
    });
    cmark_syntax_extension_set_can_contain_func(ext, [](cmark_syntax_extension*, cmark_node* node,
                                                        cmark_node_type child_type) -> int {
        if (cmark_node_get_type(node) == node_command())
            return false;
        else if (cmark_node_get_type(node) == node_section())
        {
            if (get_section_type(node) == section_type::details)
                // can contain any block
                return (child_type & CMARK_NODE_TYPE_MASK) == CMARK_NODE_TYPE_BLOCK;
            else
                // can only contain paragraphs
                return child_type == CMARK_NODE_PARAGRAPH;
        }
        else
            return false;
    });
    cmark_syntax_extension_set_open_block_func(ext, &try_open_block);
    cmark_syntax_extension_set_postprocess_func(ext, &create_implicit_brief_details);

    cmark_syntax_extension_set_private(ext, &c, [](cmark_mem*, void*) {});

    return ext;
}

cmark_node_type standardese::comment::detail::node_command()
{
    static const auto type = cmark_syntax_extension_add_node(0);
    return type;
}

command_type standardese::comment::detail::get_command_type(cmark_node* node)
{
    assert(cmark_node_get_type(node) == node_command());
    return make_command(get_raw_command_type(node));
}

const char* standardese::comment::detail::get_command_arguments(cmark_node* node)
{
    assert(cmark_node_get_type(node) == node_command());
    return cmark_node_get_string_content(node);
}

cmark_node_type standardese::comment::detail::node_section()
{
    static const auto type = cmark_syntax_extension_add_node(0);
    return type;
}

section_type standardese::comment::detail::get_section_type(cmark_node* node)
{
    assert(cmark_node_get_type(node) == node_section());
    return make_section(get_raw_command_type(node));
}

const char* standardese::comment::detail::get_section_key(cmark_node* node)
{
    assert(cmark_node_get_type(node) == node_section());
    return cmark_node_get_string_content(node);
}
