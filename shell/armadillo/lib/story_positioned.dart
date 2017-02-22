// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:ui' show lerpDouble;

import 'package:flutter/material.dart';

import 'panel.dart';
import 'panel_resizing_model.dart';
import 'simulated_fractional.dart';
import 'story_cluster.dart';
import 'story_panels.dart';

const double _kUnfocusedStoryMargin = 1.0;
const double _kStoryMargin = 4.0;
const double _kStoryMarginWhenResizing = 24.0;
const double _kUnfocusedCornerRadius = 4.0;
const double _kFocusedCornerRadius = 8.0;

/// Positions the [child] in a [StoryPanels] within the given [currentSize] with
/// a [SimulatedFractional] based on [panel], [displayMode], and
/// [isFocused].
class StoryPositioned extends StatelessWidget {
  final DisplayMode displayMode;
  final bool isFocused;
  final Panel panel;
  final Size currentSize;
  final double focusProgress;
  final Widget child;
  final double storyBarMaximizedHeight;
  final Key childContainerKey;
  final bool clip;

  StoryPositioned({
    this.storyBarMaximizedHeight,
    this.displayMode,
    this.isFocused,
    this.panel,
    this.currentSize,
    this.focusProgress,
    this.childContainerKey,
    this.clip: true,
    this.child,
  }) {
    assert(child != null);
  }

  @override
  Widget build(BuildContext context) => new ScopedPanelResizingWidget(
      child: child,
      builder: (
        BuildContext context,
        Widget child,
        PanelResizingModel panelResizingModel,
      ) {
        EdgeInsets margins = getFractionalMargins(
          panel,
          currentSize,
          focusProgress,
          panelResizingModel.progress,
        );

        Widget fractionalChild = !clip
            ? child
            : new ClipRRect(
                borderRadius: new BorderRadius.all(
                  new Radius.circular(
                    lerpDouble(
                      _kUnfocusedCornerRadius,
                      _kFocusedCornerRadius,
                      focusProgress,
                    ),
                  ),
                ),
                child: child);

        return displayMode == DisplayMode.panels
            ? new SimulatedFractional(
                key: childContainerKey,
                fractionalTop: panel.top + margins.top,
                fractionalLeft: panel.left + margins.left,
                fractionalWidth: panel.width - (margins.left + margins.right),
                fractionalHeight: panel.height - (margins.top + margins.bottom),
                size: currentSize,
                child: fractionalChild,
              )
            // If we're not in 'panel' displaymode we're in 'tabs' display
            // mode.  When that's the case, if we're focused we expand to
            // fit the entire area, otherwise we shrink our height to the height
            // of our story bar.
            : new SimulatedFractional(
                key: childContainerKey,
                fractionalTop: 0.0,
                fractionalLeft: 0.0,
                fractionalWidth: 1.0,
                fractionalHeight: (isFocused)
                    ? 1.0
                    : storyBarMaximizedHeight / currentSize.height,
                size: currentSize,
                child: fractionalChild,
              );
      });

  static EdgeInsets getFractionalMargins(
    Panel panel,
    Size currentSize,
    double focusProgress,
    double panelResizingProgress,
  ) {
    double scale =
        lerpDouble(_kUnfocusedStoryMargin, _kStoryMargin, focusProgress) /
            _kStoryMargin;
    double scaledMargin = lerpDouble(
          _kStoryMargin,
          _kStoryMarginWhenResizing,
          panelResizingProgress,
        ) /
        2.0 *
        scale;
    double topMargin =
        panel.top == 0.0 ? 0.0 : scaledMargin / currentSize.height;
    double leftMargin =
        panel.left == 0.0 ? 0.0 : scaledMargin / currentSize.width;
    double bottomMargin =
        panel.bottom == 1.0 ? 0.0 : scaledMargin / currentSize.height;
    double rightMargin =
        panel.right == 1.0 ? 0.0 : scaledMargin / currentSize.width;
    return new EdgeInsets.only(
        top: topMargin,
        left: leftMargin,
        bottom: bottomMargin,
        right: rightMargin);
  }
}
