# ADR 0011 – FEM: Elementtyp, Löser, Validierung

Status: vorgeschlagen (M5)

## Kontext
„Lineare Tet-FEM“ ist mehrdeutig: lineare Elastizität vs. lineare Elemente (T4).
T4 (konstante Dehnung) sperrt in Biegung (Shear Locking); Konvergenz in der
Energienorm nur O(h), Kragbalken-Durchbiegung wird bei groben Netzen stark
unterschätzt. Für dünnwandige Laborteile (Klemmen, Halter) ist Biegung der
Regelfall.

## Entscheidung
- Lineare Elastizität, **T10** (quadratische Tetraeder) als Default,
  Konvergenz O(h²) in der Energienorm; T4 wählbar.
- Löser: K·u = f, PCG mit AMG-Vorkonditionierer (AMGCL, header-only).
  Jacobi-PCG als Referenz (Iterationszahl ∝ √κ(K), κ ∝ h⁻²).
  Abbruch ‖r‖/‖f‖ < 10⁻⁸.
- von Mises: σ_v = √(½[(σ₁−σ₂)² + (σ₂−σ₃)² + (σ₃−σ₁)²]), Anzeige je Element
  und an Knoten gemittelt (mit Hinweis, dass Mittelung glättet).
- Materialwerte (E, ν, α, E_max) **nur vom Nutzer**; Felder ohne Default.

## Validierung
Kragbalken Länge L, Rechteck b×h, Endlast F:
Euler-Bernoulli δ_EB = F·L³/(3·E·I), I = b·h³/12.
3D-FEM konvergiert gegen die Elastizitätslösung, die zusätzlich Schub enthält
(Timoshenko): δ_T = δ_EB + F·L/(κ·G·A), κ = 5/6, G = E/(2(1+ν)).
Relativer Schubanteil: δ_s/δ_EB = E/(4κG)·(h/L)² ≈ 0,78 %·(10h/L)² für ν = 0,3,
also 0,78 % bei L/h = 10, 0,20 % bei L/h = 20.
Akzeptanz: |δ_FEM − δ_T|/δ_T < 1 % bei L/h = 20 und Netzverfeinerung, sowie
beobachtete Konvergenzordnung ≥ 1,8 (T10) über drei Netzstufen.
