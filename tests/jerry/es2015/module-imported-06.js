/* Copyright JS Foundation and other contributors, http://js.foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// These classes are same as in class-inheritance-core-1.js

import { Dog } from "tests/jerry/es2015/module-imported-05.js";

class Doge extends Dog {
  constructor (name, barks, awesomeness) {
    super (name, barks);
    this.awesomeness = awesomeness;
  }

  hello () {
    return super.hello () + " and I'm " + (this.awesomeness > 9000 ? "super awesome" : "awesome") + ".";
  }

  whoAmI ( ) {
    return "I am a Doge.";
  }

  static speak () {
    return "Doges wow.";
  }

  static explain () {
    return super.explain () + " dance.";
  }
}

export Doge as Doggo;