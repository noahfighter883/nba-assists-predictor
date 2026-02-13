# NBA Assists Predictor (C)

Statistical projection model for forecasting NBA player assist totals using sportsbook lines, potential assists, pace, and team context.

## Overview

This project builds a weighted projection system to estimate player assists for upcoming NBA games. The model blends betting market expectations with recent usage metrics and contextual adjustments.

## Inputs

- Sportsbook assist lines
- Last-5 potential assists
- Team pace
- Offensive role context
- Opponent defensive tendencies

## Modeling Approach

The projection blends:
- Market-implied baseline
- Usage-weighted recent form
- Pace adjustments
- Context multipliers

Final projection is generated via weighted aggregation.

## How to Compile

```bash
gcc assists_model.c -o assists_model
