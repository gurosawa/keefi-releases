#pragma once
#include <QMetaType>

namespace keefi {
// Core presentation fact. No QColor, QWidget, numeric inference or risk semantics.
enum class MovementDirection { Unknown, Flat, Up, Down };
}
Q_DECLARE_METATYPE(keefi::MovementDirection)
