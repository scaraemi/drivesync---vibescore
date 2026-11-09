# SPDX-License-Identifier: GPL-3.0-only
#
# Analysis: drawings, texts and symbols over the score, in per-person layers (files next to the score,
# shared through the Drive folder). The notation view paints them and passes them its input while it is on.

muse_create_module(analysis NO_UNITY)

target_sources(analysis PRIVATE
    analysismodule.cpp
    analysismodule.h
    ianalysisservice.h

    internal/analysisdocument.cpp
    internal/analysisdocument.h
    internal/analysisservice.cpp
    internal/analysisservice.h
)

target_link_libraries(analysis PRIVATE Qt::Gui engraving)
