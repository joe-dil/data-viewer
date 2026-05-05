## Keybinds
```
'f'     column frequency analysis
'i'     highlight a row
'u'     unhighlight a row
's'     make a new sheet from highlighted rows
'tab'   switch sheets
'q'     quit sheet
'c'     go to column name
'/'     search cells for value
'n','N' next/prev search match
'[',']' sort column desc/asc
'='     clear sort and unhighlight all rows
'<','>' jump to prev/next distinct value in column
'_'     auto-expand current column width
'enter' open selected sheet (xlsx sheet-list only)
ctrl+c  force quit
ctrl+d/u  half page down/up (also PgDn/PgUp)
h,j,k,l (or arrows)  left/down/up/right
g,G (or Home/End)  top/bottom
'0','$' first/last column
```

## CLI
```
-n      file has no header row
-d X    set delimiter (single char or '\t'); auto-detected if omitted
```

## Tip
do a frequency analysis on a column, select a value with 'i'  
switch back to the parent sheet and press 's' so make a new sheet  
which will contain all values for the value from the frequency sheet
