/* nba_assists_model.c
 * NBA player assists projection with last-5 potential assists & conversion.
 *
 * Primary base:
 *   - Sportsbook assists line
 *   - Season assists average
 *
 * Adjusters (multiplicative):
 *   - Home/Away
 *   - Game Total O/U (light)
 *   - Team Total O/U (moderate)
 *   - Opponent assists allowed (def vs AST)
 *   - Pace
 *   - Recent form (last N vs season)
 *   - Minutes trend (expected vs season)
 *   - Back-to-back penalty
 *   - Potential assists (uses LAST 5 games avg potential + LAST 5 conversion)
 */

#include <stdio.h>

/*======================== TUNABLE WEIGHTS & CAPS ========================*/

/* Base blend between line and season average (should sum ~1.0) */
static const double W_BASE_LINE        = 0.55;
static const double W_BASE_SEASON_AVG  = 0.45;

/* Multipliers — tweak to taste */
static const double W_HOME_AWAY        = 0.03;  /* ~3% bump home, ~3% penalty away */
static const double W_GAME_TOTAL       = 0.05;  /* light: game O/U vs league baseline */
static const double W_TEAM_TOTAL       = 0.10;  /* moderate: team O/U vs league baseline */
static const double W_DEF_AST_ALLOWED  = 0.12;  /* opp AST allowed vs league baseline */
static const double W_PACE             = 0.06;  /* possessions vs league average */
static const double W_RECENT_FORM      = 0.08;  /* last-N AST vs season AST (relative) */
static const double W_MINUTES_TREND    = 0.10;  /* expected vs season minutes (relative) */
static const double W_BACK_TO_BACK     = 0.03;  /* fixed penalty if on B2B */
static const double W_POTENTIAL_AST    = 0.14;  /* last-5 pot.AST * conv. vs season avg */

/* Baselines (edit as you see fit) */
static const double LEAGUE_AVG_GAME_TOTAL     = 229.0;
static const double LEAGUE_AVG_TEAM_TOTAL     = 114.5;
static const double LEAGUE_AVG_PACE           = 99.5;   /* possessions per team per game */
static const double LEAGUE_AVG_AST_ALLOWED    = 25.0;   /* opponent AST allowed per game */

/* Caps to keep outputs reasonable */
static const double MULT_MIN = 0.70;
static const double MULT_MAX = 1.40;

/*======================== HELPERS ========================*/
static double clamp(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/*======================== INPUTS / OUTPUTS ========================*/
typedef struct {
    /* Core */
    const char *player_name;
    double line_ast;             /* Sportsbook assists line */
    double season_avg_ast;       /* Season assists average */

    /* Context */
    int is_home;                 /* 1 home, 0 away */
    double game_total_ou;
    double team_total_ou;
    double opp_ast_allowed;      /* Opponent assists allowed per game */

    /* Pace & usage context */
    double matchup_pace;         /* projected possessions per team */
    double recent_avg_ast;       /* last N games AST (enter season avg to neutralize) */
    double season_avg_minutes;   /* season minutes avg */
    double expected_minutes;     /* expected minutes this game */
    int is_back_to_back;         /* 1 if B2B, else 0 */

    /* Potential assists — LAST 5 GAMES */
    double last5_potential_ast;  /* avg potential assists over last 5 games */
    double last5_conversion;     /* last-5 conversion rate (0..1), e.g., 0.55 */
} Inputs;

typedef struct {
    double base_assists;

    double m_homeaway;
    double m_game_total;
    double m_team_total;
    double m_def_ast;
    double m_pace;
    double m_recent;
    double m_minutes;
    double m_b2b;
    double m_potential;

    double uncapped_multiplier;
    double final_multiplier;
    double projection;
} Output;

/*======================== MODEL FUNCTIONS ========================*/
static double base_assists(const Inputs *in) {
    return W_BASE_LINE * in->line_ast
         + W_BASE_SEASON_AVG * in->season_avg_ast;
}

static double m_homeaway(const Inputs *in) {
    return in->is_home ? (1.0 + W_HOME_AWAY) : (1.0 - W_HOME_AWAY);
}

static double m_game_total(const Inputs *in) {
    double rel = (in->game_total_ou - LEAGUE_AVG_GAME_TOTAL) / LEAGUE_AVG_GAME_TOTAL;
    return 1.0 + rel * W_GAME_TOTAL;
}

static double m_team_total(const Inputs *in) {
    double rel = (in->team_total_ou - LEAGUE_AVG_TEAM_TOTAL) / LEAGUE_AVG_TEAM_TOTAL;
    return 1.0 + rel * W_TEAM_TOTAL;
}

static double m_def_ast(const Inputs *in) {
    if (LEAGUE_AVG_AST_ALLOWED <= 0.0) return 1.0;
    double rel = (in->opp_ast_allowed - LEAGUE_AVG_AST_ALLOWED) / LEAGUE_AVG_AST_ALLOWED;
    return 1.0 + rel * W_DEF_AST_ALLOWED;
}

static double m_pace(const Inputs *in) {
    if (LEAGUE_AVG_PACE <= 0.0) return 1.0;
    double rel = (in->matchup_pace - LEAGUE_AVG_PACE) / LEAGUE_AVG_PACE;
    return 1.0 + rel * W_PACE;
}

static double m_recent(const Inputs *in) {
    if (W_RECENT_FORM == 0.0 || in->season_avg_ast <= 0.0) return 1.0;
    double rel = (in->recent_avg_ast - in->season_avg_ast) / in->season_avg_ast;
    return 1.0 + rel * W_RECENT_FORM;
}

static double m_minutes(const Inputs *in) {
    if (W_MINUTES_TREND == 0.0 || in->season_avg_minutes <= 0.0) return 1.0;
    double rel = (in->expected_minutes - in->season_avg_minutes) / in->season_avg_minutes;
    return 1.0 + rel * W_MINUTES_TREND;
}

static double m_b2b(const Inputs *in) {
    return (in->is_back_to_back && W_BACK_TO_BACK > 0.0) ? (1.0 - W_BACK_TO_BACK) : 1.0;
}

/* Potential assists (LAST 5):
 * expected_actual = last5_potential_ast * last5_conversion
 * relative lift vs season_avg_ast -> weighted into multiplier
 */
static double m_potential_assists(const Inputs *in) {
    if (W_POTENTIAL_AST == 0.0 || in->season_avg_ast <= 0.0) return 1.0;
    double expected_actual = in->last5_potential_ast * in->last5_conversion;
    double rel = (expected_actual - in->season_avg_ast) / in->season_avg_ast;
    return 1.0 + rel * W_POTENTIAL_AST;
}

static Output project(const Inputs *in) {
    Output o;
    o.base_assists = base_assists(in);

    o.m_homeaway   = m_homeaway(in);
    o.m_game_total = m_game_total(in);
    o.m_team_total = m_team_total(in);
    o.m_def_ast    = m_def_ast(in);
    o.m_pace       = m_pace(in);
    o.m_recent     = m_recent(in);
    o.m_minutes    = m_minutes(in);
    o.m_b2b        = m_b2b(in);
    o.m_potential  = m_potential_assists(in);

    o.uncapped_multiplier =
        o.m_homeaway *
        o.m_game_total *
        o.m_team_total *
        o.m_def_ast *
        o.m_pace *
        o.m_recent *
        o.m_minutes *
        o.m_b2b *
        o.m_potential;

    o.final_multiplier = clamp(o.uncapped_multiplier, MULT_MIN, MULT_MAX);
    o.projection = o.base_assists * o.final_multiplier;
    return o;
}

/*======================== I/O ========================*/
static void print_output(const Inputs *in, const Output *o) {
    printf("\nAssist Projection for %s\n", in->player_name);
    printf("----------------------------------------\n");
    printf("Base (blend)            : %.2f\n", o->base_assists);
    printf("Multipliers:\n");
    printf("  Home/Away             : %.4f\n", o->m_homeaway);
    printf("  Game Total (O/U)      : %.4f\n", o->m_game_total);
    printf("  Team Total (O/U)      : %.4f\n", o->m_team_total);
    printf("  Opp AST Allowed       : %.4f\n", o->m_def_ast);
    printf("  Pace                  : %.4f\n", o->m_pace);
    printf("  Recent Form           : %.4f\n", o->m_recent);
    printf("  Minutes Trend         : %.4f\n", o->m_minutes);
    printf("  Back-to-Back          : %.4f\n", o->m_b2b);
    printf("  Last-5 Potential AST  : %.4f\n", o->m_potential);
    printf("Uncapped Multiplier     : %.4f\n", o->uncapped_multiplier);
    printf("Final Multiplier        : %.4f  (capped to [%.2f, %.2f])\n",
           o->final_multiplier, MULT_MIN, MULT_MAX);
    printf("Projected Assists       : %.2f\n\n", o->projection);
}

int main(void) {
    Inputs in;
    static char namebuf[128];

    printf("Player name: ");
    if (!fgets(namebuf, sizeof(namebuf), stdin)) return 0;
    for (int i = 0; namebuf[i]; ++i) { if (namebuf[i] == '\n') { namebuf[i] = 0; break; } }
    in.player_name = namebuf;

    printf("Sportsbook line (assists): ");
    scanf("%lf", &in.line_ast);

    printf("Season avg assists: ");
    scanf("%lf", &in.season_avg_ast);

    printf("Is home? (1=yes, 0=no): ");
    scanf("%d", &in.is_home);

    printf("Game total O/U: ");
    scanf("%lf", &in.game_total_ou);

    printf("Team total O/U: ");
    scanf("%lf", &in.team_total_ou);

    printf("Opponent assists allowed per game: ");
    scanf("%lf", &in.opp_ast_allowed);

    printf("Projected matchup pace (possessions per team): ");
    scanf("%lf", &in.matchup_pace);

    printf("Recent avg assists (last N; enter season avg to neutralize): ");
    scanf("%lf", &in.recent_avg_ast);

    printf("Season avg minutes: ");
    scanf("%lf", &in.season_avg_minutes);

    printf("Expected minutes this game: ");
    scanf("%lf", &in.expected_minutes);

    printf("Back-to-back? (1=yes, 0=no): ");
    scanf("%d", &in.is_back_to_back);

    /* === Last-5 potential assists & conversion === */
    printf("Last-5 average potential assists: ");
    scanf("%lf", &in.last5_potential_ast);

    printf("Last-5 conversion rate on potential assists (0–1, e.g., 0.54): ");
    scanf("%lf", &in.last5_conversion);

    Output out = project(&in);
    print_output(&in, &out);

    return 0;
}
