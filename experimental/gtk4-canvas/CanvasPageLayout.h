#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <optional>
#include <vector>

namespace prototype {

struct CanvasLayoutSettings {
    std::size_t columns = 1;
    std::size_t rows = 1;
    int pairedPageOffset = 1;
    bool fixedRows = false;
    bool pairedPages = false;
    bool vertical = true;
    bool rightToLeft = false;
    bool bottomToTop = false;
    bool presentationMode = false;
};

struct CanvasGridPosition {
    std::size_t column = 0;
    std::size_t row = 0;
};

/** Page placement matching LayoutMapper and page centering in Layout.cpp. */
class CanvasPageLayout {
public:
    template <typename PageSizes>
    void configure(const PageSizes& pageSizes, const CanvasLayoutSettings& requested, double gap) {
        const std::size_t pageCount = pageSizes.size();
        positions.clear();
        pageByCell.clear();
        columnWidths.clear();
        rowHeights.clear();
        columnStarts.clear();
        rowStarts.clear();
        width = 0;
        height = 0;
        columns = 0;
        rows = 0;
        pageGap = std::isfinite(gap) ? std::max(0.0, gap) : 0.0;
        if (pageCount == 0) {
            return;
        }

        const bool paired = requested.pairedPages;
        const bool vertical = requested.presentationMode || requested.vertical;
        const std::size_t firstPageOffset =
                paired ? static_cast<std::size_t>(std::max(0, requested.pairedPageOffset)) : 0;
        const std::size_t requestedColumns = requested.presentationMode ? 1 : std::max<std::size_t>(1, requested.columns);
        const std::size_t requestedRows = requested.presentationMode ? 1 : std::max<std::size_t>(1, requested.rows);
        const bool fixedRows = !requested.presentationMode && requested.fixedRows;

        if (fixedRows) {
            rows = std::clamp<std::size_t>(requestedRows, 1, pageCount);
            columns = std::max<std::size_t>(1, ceilDivide(saturatedAdd(pageCount, firstPageOffset), rows));
            if (paired && (columns % 2) != 0) {
                ++columns;
            }
        } else {
            columns = std::clamp<std::size_t>(requestedColumns, 1, pageCount);
            if (paired && (columns % 2) != 0) {
                ++columns;
            }
            rows = std::max<std::size_t>(1, ceilDivide(saturatedAdd(pageCount, firstPageOffset), columns));
        }

        const std::size_t offsetDivisor = vertical ? (paired ? saturatedMultiply(2, rows) : rows) : columns;
        const std::size_t offset = offsetDivisor == 0 ? 0 : firstPageOffset % offsetDivisor;

        positions.reserve(pageCount);
        pageByCell.assign(saturatedMultiply(columns, rows), -1);
        columnWidths.assign(columns, 0.0);
        rowHeights.assign(rows, 0.0);

        for (std::size_t page = 0; page < pageCount; ++page) {
            const std::size_t mappedPage = saturatedAdd(page, offset);
            CanvasGridPosition position{};
            if (paired) {
                position = defaultPosition(mappedPage / 2, columns / 2, rows, vertical);
                position.column = saturatedAdd(saturatedMultiply(position.column, 2), mappedPage % 2);
            } else {
                position = defaultPosition(mappedPage, columns, rows, vertical);
            }
            if (requested.rightToLeft && position.column < columns) {
                position.column = columns - 1 - position.column;
            }
            if (requested.bottomToTop && position.row < rows) {
                position.row = rows - 1 - position.row;
            }

            positions.push_back(position);
            if (position.column >= columns || position.row >= rows) {
                continue;
            }
            const std::size_t cell = position.row * columns + position.column;
            pageByCell[cell] = static_cast<int>(page);
            columnWidths[position.column] = std::max(columnWidths[position.column], pageSizes[page].width);
            rowHeights[position.row] = std::max(rowHeights[position.row], pageSizes[page].height);
        }

        columnStarts.resize(columns);
        rowStarts.resize(rows);
        for (std::size_t column = 0; column < columns; ++column) {
            columnStarts[column] = width;
            width += columnWidths[column];
            if (column + 1 < columns) {
                width += pageGap;
            }
        }
        for (std::size_t row = 0; row < rows; ++row) {
            rowStarts[row] = height;
            height += rowHeights[row];
            if (row + 1 < rows) {
                height += pageGap;
            }
        }
    }

    std::size_t pageCount() const { return positions.size(); }
    std::size_t columnCount() const { return columns; }
    std::size_t rowCount() const { return rows; }
    double layoutWidth() const { return width; }
    double layoutHeight() const { return height; }

    CanvasGridPosition gridPosition(int page) const {
        if (page < 0 || static_cast<std::size_t>(page) >= positions.size()) {
            return {};
        }
        return positions[static_cast<std::size_t>(page)];
    }

    template <typename PageSizes>
    double pageLeft(int page, const PageSizes& pageSizes) const {
        const auto position = gridPosition(page);
        if (page < 0 || static_cast<std::size_t>(page) >= pageSizes.size() || position.column >= columns) {
            return 0;
        }
        return columnStarts[position.column] +
               0.5 * (columnWidths[position.column] - pageSizes[static_cast<std::size_t>(page)].width);
    }

    template <typename PageSizes>
    double pageTop(int page, const PageSizes& pageSizes) const {
        const auto position = gridPosition(page);
        if (page < 0 || static_cast<std::size_t>(page) >= pageSizes.size() || position.row >= rows) {
            return 0;
        }
        return rowStarts[position.row] +
               0.5 * (rowHeights[position.row] - pageSizes[static_cast<std::size_t>(page)].height);
    }

    template <typename PageSizes>
    std::optional<int> pageAt(double x, double y, const PageSizes& pageSizes) const {
        if (!std::isfinite(x) || !std::isfinite(y) || columns == 0 || rows == 0) {
            return std::nullopt;
        }
        const auto column = containingCell(x, columnStarts, columnWidths);
        const auto row = containingCell(y, rowStarts, rowHeights);
        if (!column || !row) {
            return std::nullopt;
        }
        const int page = pageByCell[*row * columns + *column];
        if (page < 0 || static_cast<std::size_t>(page) >= pageSizes.size()) {
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(page);
        const double left = pageLeft(page, pageSizes);
        const double top = pageTop(page, pageSizes);
        if (x < left || x > left + pageSizes[index].width || y < top || y > top + pageSizes[index].height) {
            return std::nullopt;
        }
        return page;
    }

private:
    static std::size_t saturatedAdd(std::size_t left, std::size_t right) {
        const auto maximum = static_cast<std::size_t>(-1);
        return right > maximum - left ? maximum : left + right;
    }

    static std::size_t saturatedMultiply(std::size_t left, std::size_t right) {
        const auto maximum = static_cast<std::size_t>(-1);
        return left != 0 && right > maximum / left ? maximum : left * right;
    }

    static std::size_t ceilDivide(std::size_t value, std::size_t divisor) {
        return value / divisor + (value % divisor != 0);
    }

    static CanvasGridPosition defaultPosition(std::size_t page, std::size_t gridColumns, std::size_t gridRows,
                                              bool vertical) {
        if (vertical) {
            return {page / gridRows, page % gridRows};
        }
        return {page % gridColumns, page / gridColumns};
    }

    static std::optional<std::size_t> containingCell(double coordinate, const std::vector<double>& starts,
                                                     const std::vector<double>& extents) {
        const auto next = std::upper_bound(starts.begin(), starts.end(), coordinate);
        if (next == starts.begin()) {
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(std::distance(starts.begin(), next) - 1);
        if (index >= extents.size() || coordinate > starts[index] + extents[index]) {
            return std::nullopt;
        }
        return index;
    }

    std::vector<CanvasGridPosition> positions;
    std::vector<int> pageByCell;
    std::vector<double> columnWidths;
    std::vector<double> rowHeights;
    std::vector<double> columnStarts;
    std::vector<double> rowStarts;
    std::size_t columns = 0;
    std::size_t rows = 0;
    double pageGap = 0;
    double width = 0;
    double height = 0;
};

}  // namespace prototype
