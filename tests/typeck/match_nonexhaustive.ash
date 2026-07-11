// A match over a sum covers every variant or ends in a catch all.

Level is either Low or Mid or High

contract Broken {
    clause pick(l: Level) -> Int {
        return match l {
            Low -> 0
            High -> 2
        }
    }
}
