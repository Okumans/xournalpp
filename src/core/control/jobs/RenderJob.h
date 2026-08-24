/*
 * Xournal++
 *
 * A job which redraws a page or a page region
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstdint>   // for uint64_t
#include <optional>  // for optional

#include <cairo.h>   // for cairo_surface_t
#include <gtk/gtk.h>  // for GtkWidget

#include "Job.h"  // for Job, JobType

class XojPageView;
namespace xoj::util {
template <class T>
class Rectangle;
}  // namespace xoj::util

class RenderJob: public Job {
public:
    RenderJob(XojPageView* view);
    RenderJob(XojPageView* view, std::uint64_t preloadGeneration);

protected:
    ~RenderJob() override = default;

public:
    JobType getType() override;

    void* getSource() override;

    void run() override;

    bool isPreload() const;
    std::optional<std::uint64_t> getPreloadGeneration() const;

private:
    void repaintPage() const;

    void repaintPageArea(double x1, double y1, double x2, double y2) const;

    void rerenderRectangle(xoj::util::Rectangle<double> const& rect);

    void renderToBuffer(cairo_t* cr) const;

private:
    XojPageView* view;
    std::optional<std::uint64_t> preloadGeneration;
};
