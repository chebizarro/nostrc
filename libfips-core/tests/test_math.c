#include <fips/proto/math.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)
static uint64_t bits(double value)
{
    uint64_t result;
    memcpy(&result, &value, sizeof(result));
    return result;
}
/* Pinned Rust core/src/proto/math.rs: base 0.5469, exponents 0..64. */
static const uint64_t exponent_bits[65] = {
    UINT64_C(4607182418800017408),
    UINT64_C(4603101256817694265),
    UINT64_C(4599059719486476564),
    UINT64_C(4595061523598637995),
    UINT64_C(4591110734581567521),
    UINT64_C(4587211799199743189),
    UINT64_C(4583369581324151087),
    UINT64_C(4579589401056875799),
    UINT64_C(4575767149467000689),
    UINT64_C(4571696298736572045),
    UINT64_C(4567666039852676548),
    UINT64_C(4563680180330519005),
    UINT64_C(4559742884830230438),
    UINT64_C(4555858708657062155),
    UINT64_C(4552032634403898040),
    UINT64_C(4548270112030834483),
    UINT64_C(4544354563360527691),
    UINT64_C(4540294275568643348),
    UINT64_C(4536275570426927606),
    UINT64_C(4532302348387966280),
    UINT64_C(4528378875766797735),
    UINT64_C(4524509819058810932),
    UINT64_C(4520700282476662320),
    UINT64_C(4516955849008155521),
    UINT64_C(4512944725975293565),
    UINT64_C(4508895258952005728),
    UINT64_C(4504888389566980846),
    UINT64_C(4500928113478687981),
    UINT64_C(4497018801138360600),
    UINT64_C(4493165232945557861),
    UINT64_C(4489372637701317579),
    UINT64_C(4485615981764614463),
    UINT64_C(4481537704404649042),
    UINT64_C(4477499322273366217),
    UINT64_C(4473504577543216416),
    UINT64_C(4469557563402425916),
    UINT64_C(4465662756980276619),
    UINT64_C(4461825055360777072),
    UINT64_C(4458049814974415819),
    UINT64_C(4454201463077140889),
    UINT64_C(4450133567379621914),
    UINT64_C(4446106540710723012),
    UINT64_C(4442124216585328747),
    UINT64_C(4438190788102019851),
    UINT64_C(4434310841672023934),
    UINT64_C(4430489393911941751),
    UINT64_C(4426731931997011213),
    UINT64_C(4422789645938667307),
    UINT64_C(4418732385308890979),
    UINT64_C(4414716991277088983),
    UINT64_C(4410747390930151312),
    UINT64_C(4406827879715718241),
    UINT64_C(4402963155994418700),
    UINT64_C(4399158358833108615),
    UINT64_C(4395419109343114996),
    UINT64_C(4391380596291120464),
    UINT64_C(4387334230319735710),
    UINT64_C(4383330752865282417),
    UINT64_C(4379374186870648759),
    UINT64_C(4375468932630765815),
    UINT64_C(4371619803188229206),
    UINT64_C(4367832063049030013),
    UINT64_C(4364049754725915957),
    UINT64_C(4359974381685999134),
    UINT64_C(4355939176299985466),
};
/* Faithful port of the single upstream math test: all golden bits plus the
 * full 12-base, 0..64 exponent relative-tolerance sweep against host libm. */
static int powi_deterministic_and_close(void)
{
    const struct { double base; uint32_t exp; uint64_t expected; } golden[] = {
        {0.5469, 4, UINT64_C(4591110734581567521)},
        {0.5, 2, UINT64_C(4598175219545276416)},
        {2.0, 10, UINT64_C(4652218415073722368)},
        {0.9999, 64, UINT64_C(4607124953935219716)},
        {1.5, 8, UINT64_C(4627907113471967232)},
        {3.7, 5, UINT64_C(4649310774781981293)},
        {0.1, 3, UINT64_C(4562254508917369341)},
        {0.5493, 4, UINT64_C(4591224636877479401)},
        {0.5508, 4, UINT64_C(4591296588123960286)},
        {0.5469, 14, UINT64_C(4552032634403898040)}};
    for (size_t i = 0; i < sizeof(golden)/sizeof(golden[0]); ++i)
        CHECK(bits(fips_powi(golden[i].base, golden[i].exp)) == golden[i].expected);
    const double bases[] = {0.0, 1.0, 0.5, 0.5469, 0.5493, 0.5508, 0.9999, 1.5, 2.0, 3.7, 0.1, 1e-3};
    for (size_t i = 0; i < sizeof(bases)/sizeof(bases[0]); ++i) {
        for (uint32_t exp = 0; exp <= 64; ++exp) {
            double ours = fips_powi(bases[i], exp), host = pow(bases[i], (double)exp);
            if (host == 0) CHECK(ours == 0);
            else CHECK(fabs((ours - host) / host) <= 1e-11);
        }
    }
    return 0;
}
int main(void)
{
    CHECK(powi_deterministic_and_close() == 0);
    for (uint32_t exp = 0; exp <= 64; ++exp)
        CHECK(bits(fips_powi(0.5469, exp)) == exponent_bits[exp]);
    CHECK(fips_powi(NAN, 0) == 1 && fips_powi(INFINITY, 0) == 1);
    CHECK(bits(fips_powi(-0.0, 3)) == UINT64_C(0x8000000000000000));
    CHECK(bits(fips_powi(-0.0, 2)) == 0);
    CHECK(fips_powi(-2, 3) == -8 && fips_powi(-2, 4) == 16);
    CHECK(isinf(fips_powi(2, UINT32_MAX)));
    CHECK(fips_powi(0.5, UINT32_MAX) == 0);
    CHECK(fips_powi(1, UINT32_MAX) == 1);
    CHECK(bits(fips_powi(2, 0)) == UINT64_C(0x3ff0000000000000));
    CHECK(bits(fips_powi(0.5, 1074)) == 1); /* Gradual underflow, not FTZ. */
    printf("1 upstream math test passed; 10 golden pins, 780 tolerance cases, 65 Rust exponent pins; C edges passed\n");
    return 0;
}
