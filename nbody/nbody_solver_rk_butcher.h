#ifndef NBODY_SOLVER_RK_BUTCHER_H
#define NBODY_SOLVER_RK_BUTCHER_H

#include "nbody_solver.h"
#include "nbody_butcher_table.h"

class NBODY_DLL nbody_solver_rk_butcher : public nbody_solver
{
	nbody_butcher_table*		m_bt;
	nbody_engine::memory_array	m_k;
	nbody_engine::memory*		m_t;
	nbody_engine::memory*		m_tmpk;
	nbody_engine::memory*		m_ycorr_data;
	nbody_engine::memory*		m_tcorr_data;
	nbody_engine::memory_array	m_y_stack;

	size_t						m_max_recursion;
	size_t						m_substep_subdivisions;
	nbcoord_t					m_error_threshold;
	size_t						m_refine_steps_count;
	bool						m_correction;
public:
	explicit nbody_solver_rk_butcher(nbody_butcher_table*);
	~nbody_solver_rk_butcher();
	void set_max_recursion(size_t);
	void set_substep_subdivisions(size_t);
	void set_error_threshold(nbcoord_t);
	void set_refine_steps_count(size_t);
	void set_correction(bool corr);
	const char* type_name() const override;
	void advise(nbcoord_t dt) override;
	void print_info() const override;
	void reset() override;

	const nbody_butcher_table* table() const;
private:
	void sub_step(size_t substeps_count, nbcoord_t t, nbcoord_t dt,
				  nbody_engine::memory* y, size_t recursion_level);
	void sub_step_implicit(size_t steps, const nbcoord_t** a,
						   nbcoord_t* coeff,
						   const nbody_engine::memory* y,
						   bool need_first_approach_k,
						   const nbcoord_t* c, nbcoord_t t,
						   nbcoord_t dt);
	void sub_step_explicit(size_t steps, const nbcoord_t** a,
						   nbcoord_t* coeff,
						   const nbody_engine::memory* y,
						   const nbcoord_t* c, nbcoord_t t,
						   nbcoord_t dt);
};

#endif // NBODY_SOLVER_RK_BUTCHER_H
