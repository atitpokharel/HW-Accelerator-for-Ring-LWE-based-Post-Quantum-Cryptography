#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h> 
//addresses
#define AVALON_BRIDGE_BASE 0xFF200000
#define RING_LWE_OFFSET 0x00000200   
//so the component base becomes 
#define RING_LWE_BASE (AVALON_BRIDGE_BASE + RING_LWE_OFFSET)
#define TIMER_MM 0xFF202000 //timer base address, 100 Mhz

//registers offsets, so, when r/w, I can do like: ring_lwe_base + 4*(reg_a_base_idx + some offset)
#define REG_A_BASE_IDX 0 
#define REG_S_BASE_IDX 16
#define REG_CTRL_IDX 32
#define REG_P_BASE_IDX 33
#define N 16 //n is 16
#define Q 257 //modulus=257, modulus 17 became too small for the error I used

//to read and write, simple inline functions
static inline void mmio_write(uint32_t addr, uint32_t data) { //inp args: address and data
    volatile uint32_t *p = (uint32_t *)addr; 
    *p = data; //put data in that address
}
static inline uint32_t mmio_read(uint32_t addr) { //similarly read from the address
    volatile uint32_t *p = (uint32_t *)addr;
    return *p; //return data, return type uint32
}

//now funciton for hardware
uint32_t timed_ring_mult16_hw(const int16_t A[N], const int16_t S[N], int32_t P[N]){ //making it general so, I can reuse it for keygen and enc both
    volatile uint32_t *ptr_to_timer = (uint32_t *)TIMER_MM; //timer address
    uint32_t last_count, total_cycles; //vars for timer
    int i; //had to add this declaration, was getting compilation errors
    *(ptr_to_timer + 3)= 0xFFFF; //max value
    *(ptr_to_timer + 2)= 0xFFFF;
    *(ptr_to_timer + 1)= 0x4; // start 
   
    //Putting A in reg A and S in reg S
    for(i = 0; i<N; i++) { //so, iterate over 16 values andput one by one
        mmio_write(RING_LWE_BASE + 4*(REG_A_BASE_IDX + i), (uint16_t)A[i]);} //16 bit values
    for(i = 0; i<N; i++) {
        mmio_write(RING_LWE_BASE + 4*(REG_S_BASE_IDX + i), (uint16_t)S[i]);}
    //timer setup

    mmio_write(RING_LWE_BASE + 4 * REG_CTRL_IDX, 0x1); //start the accelerator

    //to check the calc is done or not, polling the stat register
    volatile uint32_t *status_reg = (uint32_t *)(RING_LWE_BASE + 4 * REG_CTRL_IDX); //address
    while (!(*status_reg & 0x2)) { //can see 10 if calc is done
        //if this branch is taken, fsm is busy, wait
    }
    //out of the loop means, calc done
    *(ptr_to_timer + 1) = 0x8;//stop timer

    //now, reading the result
    for(i = 0; i < N; i++) {
        P[i] = (int32_t)mmio_read(RING_LWE_BASE + 4 * (REG_P_BASE_IDX + i));
    }
    //time calculation as always
    *(ptr_to_timer + 4) = 1; 
    last_count = (*(ptr_to_timer + 5) << 16) | *(ptr_to_timer + 4);//shift and concat two 16 bit values
    total_cycles = 0xFFFFFFFF - last_count; //diff is the total cycles
    return total_cycles;
}//so, result is returned via mmio read, and total cycles via software in this function
 
  
//sw based implementation
uint32_t timed_ring_mult16_sw(const int16_t A[N], const int16_t S[N], int32_t P[N]) { //same args
    volatile uint32_t *ptr_to_timer = (uint32_t *)TIMER_MM; //same timer
    uint32_t last_count, total_cycles;
    int i, j, k; //indices
    int32_t prod; //product, a slack var

    *(ptr_to_timer + 3) = 0xFFFF; //max val
    *(ptr_to_timer + 2) = 0xFFFF;
    *(ptr_to_timer + 1) = 0x4;//start timer

    //init result to 0
    for (i = 0; i < N; i++) P[i] = 0;
    //so outer loop, iterate over values of A
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) { //inner loop, iterate over values of S
            k = i + j; //sum of indices is k, used for condition check
            prod = (int32_t)A[i] * (int32_t)S[j]; //product is required, anyways, so compute it
            //now based on conditions, add or sub the product to accumulated result
            if (k < N) { //if k is less than N, add the product to P[k]
                P[k] += prod;
            } else { //if k is greater than or equal to N, sub the product from P[k - N]
                P[k - N] -= prod; // Wrap around with negation
            }
        }
    }//same for all values of A
    
    *(ptr_to_timer + 1) = 0x8; //stop timer
    *(ptr_to_timer + 4) = 1; //latch
    last_count = (*(ptr_to_timer + 5) << 16) | *(ptr_to_timer + 4); //shift and concat two 16 bit values
    total_cycles = 0xFFFFFFFF - last_count; //diff is the total cycles

    return total_cycles; //return total cycles
}
 
//encryption - args: public key a, public key p, message m, random r, error e1, error e2, output u, output v
uint32_t timed_ring_encrypt_hw(const int16_t pk_a[N], const int16_t pk_p[N], const int16_t m[N], const int16_t r[N], const int16_t e1[N], const int16_t e2[N],
                      int32_t u[N], int32_t v[N]) {
    int32_t ar[N];
    int32_t pr[N];
    int i;
    int32_t m_scaled; //scaled message
    uint32_t cycles_ar, cycles_pr; //ar: for u, pr: for v

    //u = a*r + e1 (Hardware Mult) and mod Q
    cycles_ar = timed_ring_mult16_hw(pk_a, r, ar); //use accelerator
    for (i = 0; i < N; i++) {
        u[i] = ar[i] + e1[i];
        u[i] = ((u[i] % Q) + Q) % Q;} //apply modulo Q
    //v = p*r + e2 + m_scaled (Hardware Mult) and mod Q, scale m by Q/2
    cycles_pr = timed_ring_mult16_hw(pk_p, r, pr); //use accelerator
    for (i = 0; i < N; i++) {
        m_scaled = m[i] * (Q/2); //scale message by Q/2 = 8
        v[i] = pr[i] + e2[i] + m_scaled;
        v[i] = ((v[i] % Q) + Q) % Q;} 
    return cycles_ar + cycles_pr; //total cycles
}
//same for SW, same args
uint32_t timed_ring_encrypt_sw(const int16_t pk_a[N], const int16_t pk_p[N], const int16_t m[N],const int16_t r[N], const int16_t e1[N], const int16_t e2[N],
                      int32_t u[N], int32_t v[N]) {
    int32_t ar[N];
    int32_t pr[N];
    int i;
    int32_t m_scaled; //scaled message
    uint32_t cycles_ar, cycles_pr;
    // u = a*r + e1 (Software Mult) and mod Q
    cycles_ar = timed_ring_mult16_sw(pk_a, r, ar); //use software
    for (i = 0; i < N; i++) {
        u[i] = ar[i] + e1[i];
        u[i] = ((u[i] % Q) + Q) % Q;}
    // v = p*r + e2 + m_scaled (Software Mult) and mod Q, scale m by Q/2
    cycles_pr = timed_ring_mult16_sw(pk_p, r, pr);
    for (i = 0; i < N; i++) {
        m_scaled = m[i] * (Q/2); //scale message by Q/2 = 8
        v[i] = pr[i] + e2[i] + m_scaled;
        v[i] = ((v[i] % Q) + Q) % Q;}
    return cycles_ar + cycles_pr;
}

//decryption - args: secret key s, ciphertext u, ciphertext v, output decrypted message m
uint32_t timed_ring_decrypt_hw(const int16_t s[N], const int32_t u[N], const int32_t v[N], int32_t m_dec[N]) {
    int32_t su[N]; //s*u
    int i;
    int16_t u16[N]; //need to convert u to int16 for hw function
    int32_t noisy_m; //noisy message before rounding
    uint32_t cycles;
    
    //convert u to int16
    for(i=0; i<N; i++) u16[i] = (int16_t)u[i];
    
    //compute s*u using hardware
    cycles = timed_ring_mult16_hw(s, u16, su);
    
    //m' = v - s*u (mod Q), then round to recover message bit
    for(i=0; i<N; i++) {
        noisy_m = v[i] - su[i];
        noisy_m = ((noisy_m % Q) + Q) % Q; //apply modulo Q
        //round: if noisy_m is close to 0 or Q, output 0; if close to Q/2, output 1
        m_dec[i] = ((noisy_m + Q/4) / (Q/2)) % 2; //rounding to nearest bit
    }
    return cycles; //return cycles
}

//decryption SW - same args
uint32_t timed_ring_decrypt_sw(const int16_t s[N], const int32_t u[N], const int32_t v[N], int32_t m_dec[N]) {
    int32_t su[N]; //s*u
    int i;
    int16_t u16[N]; //need to convert u to int16 for sw function
    int32_t noisy_m; //noisy message before rounding
    uint32_t cycles;
    
    //convert u to int16
    for(i=0; i<N; i++) u16[i] = (int16_t)u[i];
    
    //compute s*u using software
    cycles = timed_ring_mult16_sw(s, u16, su);
    
    //m' = v - s*u (mod Q), then round to recover message bit
    for(i=0; i<N; i++) {
        noisy_m = v[i] - su[i];
        noisy_m = ((noisy_m % Q) + Q) % Q; //apply modulo Q
        //round: if noisy_m is close to 0 or Q, output 0; if close to Q/2, output 1
        m_dec[i] = ((noisy_m + Q/4) / (Q/2)) % 2; //rounding to nearest bit
    }
    return cycles; //return cycles
}

//print function for result arrays, needed this, because a lot of printing is required
void print_array(const char *name, const int32_t *p, int n) { //so it is like: print array name, iterate ove values and print 'em
    int i;
    printf("%s = [", name); //name of the array
    for (i = 0; i < n; i++) {
        printf("%ld", p[i]);
        if (i < n - 1) printf(", ");} //comma and space between values
    printf("]\n");} //line change at end


//main
int main(void) {
    int i;
    int16_t A[N] = {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32};
    int16_t S[N] = {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    int16_t e[N] = {-1, 0, 1, -1, 0, 1, -1, 0, 1, -1, 0, 1, -1, 0, 1, -1};

    int32_t P_hw[N]; //result array for hardware
    int32_t P_sw[N]; //result array for software
    uint32_t total_cycles_keygen_hw, total_cycles_keygen_sw;
    uint32_t total_cycles_encrypt_hw, total_cycles_encrypt_sw; //total cycles for hardware and software
    uint32_t total_cycles_decrypt_hw, total_cycles_decrypt_sw; //total cycles for decryption

    //key generation part
    //prnt the generated values first
    printf("Key Generation:\n");
    printf("A = ["); for(i=0; i<N; i++) printf("%d%s", A[i], (i<N-1)?", ":""); printf("]\n");
    printf("S = ["); for(i=0; i<N; i++) printf("%d%s", S[i], (i<N-1)?", ":""); printf("]\n");
    printf("Error e: = ["); for(i=0; i<N; i++) printf("%d%s", e[i], (i<N-1)?", ":""); printf("]\n");
    printf("Calculating in Hardware (a*s + e)...\n");
    //now calculate the result using hardware acc
    total_cycles_keygen_hw = timed_ring_mult16_hw(A, S, P_hw);
    for(i=0; i<N; i++) {
        P_hw[i] += e[i]; //add error
        P_hw[i] = ((P_hw[i] % Q) + Q) % Q; //apply modulo Q
    }//now my public key is ready
    printf("Done in HW.\n\n");

    printf("Calculating in Software (a*s + e)...\n"); //same for software
    total_cycles_keygen_sw = timed_ring_mult16_sw(A, S, P_sw);
    for(i=0; i<N; i++) {
        P_sw[i] += e[i]; //add error
        P_sw[i] = ((P_sw[i] % Q) + Q) % Q; //apply modulo Q
    }
    printf("Done.\n\n");

    printf("Results (B)\n"); //print the results
    print_array("Result p (Hardware)", P_hw, N);
    printf("Cycles = %lu\n", (unsigned long)total_cycles_keygen_hw);
    print_array("Result p (Software)", P_sw, N);
    printf("Cycles = %lu\n", (unsigned long)total_cycles_keygen_sw);


    //now, that we have public key, we can do encryption
    const int16_t *pk_a = A;
    // here, the SW result as the public key P
    int16_t pk_p[N];
    for(i=0; i<N; i++) pk_p[i] = (int16_t)P_sw[i]; //my new array pk_p (say at encryption time) is the public key

    int32_t u_hw[N], v_hw[N]; //output u and v for hardware
    int32_t u_sw[N], v_sw[N]; //output u and v for software

    //test data, random
    int16_t msg[N] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    int16_t r[N] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; 
    int16_t e1[N] = {-1, 0, 1, -1, 0, 1, -1, 0, 1, -1, 0, 1, -1, 0, 1, -1}; 
    int16_t e2[N] = {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0}; 

    printf("\nEncryption Step\n");
    printf("Error e1: = ["); for(i=0; i<N; i++) printf("%d%s", e1[i], (i<N-1)?", ":""); printf("]\n");
    printf("Error e2: = ["); for(i=0; i<N; i++) printf("%d%s", e2[i], (i<N-1)?", ":""); printf("]\n");
    printf("Random r: = ["); for(i=0; i<N; i++) printf("%d%s", r[i], (i<N-1)?", ":""); printf("]\n");
    printf("Message m: = ["); for(i=0; i<N; i++) printf("%d%s", msg[i], (i<N-1)?", ":""); printf("]\n");
    printf("\n");
    total_cycles_encrypt_hw = timed_ring_encrypt_hw(pk_a, pk_p, msg, r, e1, e2, u_hw, v_hw); //use accelerator
    total_cycles_encrypt_sw = timed_ring_encrypt_sw(pk_a, pk_p, msg, r, e1, e2, u_sw, v_sw); //use software

    printf("Results: \n"); //print the results
    print_array("Ciphertext u (HW)", u_hw, N);
    print_array("Ciphertext v (HW)", v_hw, N);
    printf("Total HW cycles = %lu\n\n", (unsigned long)total_cycles_encrypt_hw);

    print_array("Ciphertext u (SW)", u_sw, N);
    print_array("Ciphertext v (SW)", v_sw, N);
    printf("Total SW cycles = %lu\n\n", (unsigned long)total_cycles_encrypt_sw);

    //now decryption part
    int32_t m_dec_hw[N]; //decrypted message using hardware
    int32_t m_dec_sw[N]; //decrypted message using software
    
    printf("\nDecryption Step\n");
    printf("Using Hardware...\n");
    total_cycles_decrypt_hw = timed_ring_decrypt_hw(S, u_hw, v_hw, m_dec_hw); //use accelerator
    printf("Done.\n");
    
    printf("Using Software...\n");
    total_cycles_decrypt_sw = timed_ring_decrypt_sw(S, u_sw, v_sw, m_dec_sw); //use software
    printf("Done.\n\n");
    
    printf("Decryption Results:\n");
    print_array("Decrypted m (HW)", m_dec_hw, N);
    printf("Cycles = %lu\n", (unsigned long)total_cycles_decrypt_hw);
    print_array("Decrypted m (SW)", m_dec_sw, N);
    printf("Cycles = %lu\n\n", (unsigned long)total_cycles_decrypt_sw);
    
    //verify decryption - compare with original message
    int hw_match = 1, sw_match = 1;
    for(i=0; i<N; i++) {
        if(m_dec_hw[i] != msg[i]) hw_match = 0;
        if(m_dec_sw[i] != msg[i]) sw_match = 0;
    }
    
    if(hw_match) printf("SUCCESS: HW decryption matches original message!\n");
    else printf("ERROR: HW decryption does NOT match original message.\n");
    
    if(sw_match) printf("SUCCESS: SW decryption matches original message!\n");
    else printf("ERROR: SW decryption does NOT match original message.\n");

    printf("\nDone\n");

    return 0;
}

