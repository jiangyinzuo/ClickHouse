#pragma once

#include <Common/Exception.h>

#include <Analyzer/IQueryTreeNode.h>


namespace DB
{

/** Visit query tree in depth.
  * Matcher need to define `visit`, `needChildVisit` methods and `Data` type.
  */
template <typename Matcher, bool top_to_bottom, bool need_child_accept_data = false>
class InDepthQueryTreeVisitor
{
public:
    using Data = typename Matcher::Data;

    /// Initialize visitor with matchers data
    explicit InDepthQueryTreeVisitor(Data & data_)
        : data(data_)
    {}

    /// Visit query tree node
    void visit(QueryTreeNodePtr & query_tree_node)
    {
        if constexpr (!top_to_bottom)
            visitChildren(query_tree_node);

        try
        {
            Matcher::visit(query_tree_node, data);
        }
        catch (Exception & e)
        {
            e.addMessage("While processing {}", query_tree_node->formatASTForErrorMessage());
            throw;
        }

        if constexpr (top_to_bottom)
            visitChildren(query_tree_node);
    }

private:
    Data & data;

    void visitChildren(QueryTreeNodePtr & expression)
    {
        for (auto & child : expression->getChildren())
        {
            if (!child)
                continue;

            bool need_visit_child = false;
            if constexpr (need_child_accept_data)
                need_visit_child = Matcher::needChildVisit(expression, child, data);
            else
                need_visit_child = Matcher::needChildVisit(expression, child);

            if (need_visit_child)
                visit(child);
        }
    }
};

}
