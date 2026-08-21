module app.lib;
import std;

export triple: (n: int) -> int = n * 3;

export Vec: type = {
    items: list<int> = {};
    total: () -> int = {
        t: int := 0;
        for v in items {
            t += v;
        }
        return t;
    }
}
