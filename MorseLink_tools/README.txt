MorseLink - report figures and tables
=====================================

What is here
1. make_figures.py - reads every test folder and writes every figure and
   table in the report's Results chapter, plus numbers.txt (each number quoted
   in the text, with the file it came from).
2. analyse_tests.py - your session reader, unchanged. make_figures.py uses it,
   so keep the two files in the same folder.
3. participants_TEMPLATE.csv - the questionnaire answers, one row per
   participant.

How to run it (Windows)
1. Once: pip install matplotlib
2. Put make_figures.py and analyse_tests.py in the Results folder, next to
   Baseline, Dynamic 1, Point sweep, IMU, Meter, RundownB and the rest.
3. Double-click make_figures.py, or run:  python make_figures.py
   It takes about half a minute. It writes figures\, tables\ and numbers.txt
   next to itself, replacing the old ones.
4. Copy figures\*.pdf into the report's figures folder and tables\*.tex into
   its tables folder, then rebuild the report.

The questionnaires
1. Copy participants_TEMPLATE.csv to participants.csv (any folder under
   Results) and fill in one row per participant (P1, P2, ...).
2. Columns q2_1 ... q6_2 are the questionnaire's numbered items. For the
   agree/disagree items write 1-5 (1 strongly disagree, 5 strongly agree).
   For 2.5 (strength) write 1-5 (1 too weak, 3 about right, 5 too strong); for
   5.7 (usefulness of the display) 1-5 (1 not useful, 5 essential).
3. q1_1, q1_2 and q5_8 are multiple choice: write the ticked option in words.
   q6_3, q6_4 and q6_5 are the open answers: copy them as written (put the text
   in double quotes if it contains a comma).
4. speed_q, strength_i and dash_pref are the settings each participant chose
   (for example 4, 3 and H2). notes is anything else.
5. Leave a cell empty if a question was not answered.
6. Run make_figures.py again: it adds tables\questionnaire.tex, which the
   report's user-sessions section reads.

Notes
- Motor recordings: phyphox folders named M1, M2 and Strength (first test),
  M2_v064 (the replacement M2 on the first pattern), and M1_all, M2_all (the
  I1-I9 strength sweep, firmware 0.6.6). The folder name tells the script
  which pattern was played.
- Label typos in the session files can be fixed without touching them: add a
  corrections.txt to the folder (the format is described in analyse_tests.py).
- Two choices are written at the top of the point-test part of
  make_figures.py: which runs are the clear references for the obstacles, and
  the one run left out (the base board restarted at the start).
