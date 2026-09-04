# scripts

| Script | Purpose |
|---|---|
| `fake_console.py` | Stands in for the Wii client. Speaks the binary UDP protocol, pushes and pulls a card, can inject packet loss and reordering. Build this during M4. |
| `make_fixture.py` | Generates synthetic but structurally valid memory card images for tests when a real Nintendont dump is not available. Build this during M2. |
