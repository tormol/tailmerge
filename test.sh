#!/usr/bin/env bash
set -e

# This script can be ran from `make test`

# empty or no values
./test_heap assert '' '' '' 0
./test_heap assert , '' 1 1

# 1-4
./test_heap assert = = 1 1
./test_heap assert x,y, x,y 1,2 2
./test_heap assert y,x, x,y 2,1 2
./test_heap assert x,y,z x,y,z 1,2,3 3
./test_heap assert x,z,y x,y,z 1,3,2 3
./test_heap assert y,x,z x,y,z 2,1,3 3
./test_heap assert y,z,x x,y,z 3,1,2 3
./test_heap assert z,x,y x,y,z 2,3,1 3
./test_heap assert z,y,x x,y,z 3,2,1 3
./test_heap assert a,b,c,d a,b,c,d 1,2,3,4 4
./test_heap assert d,c,b,a a,b,c,d 4,3,2,1 4
./test_heap assert a,b,c,d,e a,b,c,d,e 1,2,3,4,5 5
./test_heap assert a,b,c,d,e,f a,b,c,d,e,f 1,2,3,4,5,6 6
./test_heap assert a,b,c,d,e,f,g a,b,c,d,e,f,g 1,2,3,4,5,6,7 7
./test_heap assert a,b,c,d,e,f,g,h a,b,c,d,e,f,g,h 1,2,3,4,5,6,7,8 8
./test_heap assert a,b,c,d,e,f,g,h,i a,b,c,d,e,f,g,h,i 1,2,3,4,5,6,7,8,9 9
./test_heap assert "$(seq 0 9 )" "$(seq 0 9)" "$(seq 1 10)"
./test_heap assert "$(seq 0 9 | tac)" "$(seq 0 9)"

# different length
./test_heap assert app,apple,applejuice app,apple,applejuice 1,2,3
./test_heap assert app,applejuice,apple app,apple,applejuice 1,3,2
./test_heap assert applejuice,apple,app app,apple,applejuice 3,2,1
./test_heap assert applejuice,app,apple app,apple,applejuice 2,3,1
./test_heap assert e,ef,eff,effe,effer,efferv,efferve,efferves,effervesc,effervesce,effervescen,effervescent \
                   e,ef,eff,effe,effer,efferv,efferve,efferves,effervesc,effervesce,effervescen,effervescent
./test_heap assert effervescent,effervescen,effervesce,effervesc,efferves,efferve,efferv,effer,effe,eff,ef,e \
                   e,ef,eff,effe,effer,efferv,efferve,efferves,effervesc,effervesce,effervescen,effervescent
./test_heap assert efferv,effer,effe,eff,ef,e,effervescent,effervescen,effervesce,effervesc,efferves,efferve \
                   e,ef,eff,effe,effer,efferv,efferve,efferves,effervesc,effervesce,effervescen,effervescent

# long
if [ -f /usr/share/dict/words ]; then
    ./test_heap assert "$(tail +50 /usr/share/dict/words | head -30)" \
                       "$(tail +50 /usr/share/dict/words | head -30)"
    ./test_heap assert "$(tail +50 /usr/share/dict/words | head -30 | tac)" \
                       "$(tail +50 /usr/share/dict/words | head -30)"
    ./test_heap assert "$(tail +50 /usr/share/dict/words | head -30 | shuf)" \
                       "$(tail +50 /usr/share/dict/words | head -30)"
    ./test_heap assert "$(tail +50 /usr/share/dict/words | head -30 | shuf)" \
                       "$(tail +50 /usr/share/dict/words | head -30)"
fi

# stability
./test_heap assert ,, ,, 2,1 2
./test_heap assert foo,foo foo,foo 2,1 2
./test_heap assert foo,foo,bar bar,foo,foo 3,2,1 3
./test_heap assert foo,bar,foo bar,foo,foo 2,3,1 3
./test_heap assert bar,foo,foo bar,foo,foo 1,3,2 3
./test_heap assert foo,bar,bar bar,bar,foo 3,2,1 3
./test_heap assert bar,foo,bar bar,bar,foo 3,1,2 3
./test_heap assert bar,bar,foo bar,bar,foo 2,1,3 3

# pop then push
./test_heap assert d-c-b-a d,c,b,a 1,2,3,4 4
./test_heap assert u,x-y,w--a,b u,w,x,a,b,y 1,4,2,5,6,3 6

# pop-then-push stability
./test_heap assert d,b-d,e--b-a b,d,d,b,a,e 2,3,1,5,6,4 6
