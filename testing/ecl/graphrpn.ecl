/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

//nothor
//nothorlcr

stepRecord := 
            record
string          next{maxlength(20)};
unsigned        leftStep;
unsigned        rightStep;
            end;

    
stateRecord :=
            record
unsigned        step;
unsigned        value;
            end;

mkValue(unsigned value) := transform(stepRecord, self.next := (string)value; self.leftStep := -1; self.rightStep := -1;);
mkOp(string1 op, unsigned l, unsigned r) := transform(stepRecord, self.next := op; self.leftStep := l; self.rightStep := r);

processExpression(dataset(stepRecord) actions) := function

    processNext(set of dataset(stateRecord) in, unsigned step, stepRecord action) := function
        thisLeft := in[action.leftStep];
        thisRight := in[action.rightStep];

        newValue := case(action.next,
                           '+'=>thisLeft[1].value + thisRight[1].value,
                           '*'=>thisLeft[1].value * thisRight[1].value,
                           '/'=>thisLeft[1].value / thisRight[1].value,
                           '-'=>thisLeft[1].value - thisRight[1].value,
                           '~'=>-thisLeft[1].value,
                           (unsigned)action.next);

        return dataset(row(transform(stateRecord, self.step := step; self.value := newValue)));
    END;
                
    initial := dataset([], stateRecord);
    result := GRAPH(initial, count(actions), processNext(rowset(left), counter, actions[counter]));
    return result[1].value;
end;

actions := dataset([mkValue(10), mkValue(20), mkOp('*', 1, 2), 
                    mkValue(15), mkValue(10), mkOp('+', 4, 5), mkOp('-', 3, 6)]);
output(processExpression(global(actions,few)));
