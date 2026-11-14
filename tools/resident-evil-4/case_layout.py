# SPDX-License-Identifier: GPL-3.0-or-later
"""Square-cell Case geometry; mirrored by re4_case_layout.h and checked by tests."""
CAPACITIES = ((10, 6), (11, 7), (12, 8), (15, 8))
LEFT, TOP, WIDTH, HEIGHT = 20, 104, 1200, 768
NAME_Y = 888
ART_WIDTH, ART_HEIGHT = 1080, 720


def layout(cols, rows):
    cell = min(WIDTH // cols, HEIGHT // rows) // 2 * 2
    return cell, (1240 - cols * cell) // 2, TOP + (HEIGHT - rows * cell) // 2
