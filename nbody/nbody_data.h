#ifndef NBODY_DATA_H
#define NBODY_DATA_H

#include <vector>
#include "nbtype.h"

#define NBODY_DATA_BLOCK_SIZE	64
#define NBODY_MIN_R				1e-8

class nbody_fcompute;

class nbody_data
{
	size_t						m_count;
	nbcoord_t					m_time;
	size_t						m_step;
	std::vector< nbvertex_t >	m_vertites;
	std::vector< nbcolor_t >	m_color;
	std::vector< nbvertex_t >	m_velosites;
	std::vector< nbcoord_t >	m_mass;
	std::vector< nbcoord_t >	m_a;

	nbvertex_t					m_total_impulce;
	nbvertex_t					m_total_impulce_moment;
	nbvertex_t					m_mass_center;
	nbcoord_t					m_total_kinetic_energy;
	nbcoord_t					m_total_potential_energy;
	nbvertex_t					m_last_total_impulce;
	nbvertex_t					m_last_total_impulce_moment;
	nbvertex_t					m_last_mass_center;
	nbcoord_t					m_last_total_kinetic_energy;
	nbcoord_t					m_last_total_potential_energy;
	double						m_timer_start;
	size_t						m_timer_step;

public:
	nbody_data();

	nbvertex_t force( const nbvertex_t& v1, const nbvertex_t& v2, nbcoord_t mass1, nbcoord_t mass2 ) const;
	nbcoord_t potential_energy( const nbvertex_t* vertites, size_t body1, size_t body2 ) const;
	void add_body( const nbvertex_t& r, const nbvertex_t& v, const nbcoord_t& m, const nbcoord_t& a, const nbcolor_t& color );
	void advise_time( nbcoord_t dt );
	nbcoord_t get_time() const;
	size_t get_step() const;
	nbvertex_t* get_vertites();
	const nbvertex_t* get_vertites() const;
	nbvertex_t* get_velosites();
	const nbvertex_t* get_velosites() const;
	const nbcoord_t* get_mass() const;
	const nbcolor_t* get_color() const;

	void print_statistics( nbody_fcompute* );
	void dump_body( size_t n );
	size_t get_count() const;

	void add_galaxy( nbvertex_t center, nbvertex_t velosity, nbcoord_t radius, nbcoord_t total_mass, size_t count, nbcolor_t& color );
	void make_universe( nbcoord_t sx, nbcoord_t sy, nbcoord_t sz );

	nbvertex_t total_impulce() const;
	nbvertex_t total_impulce_moment() const;
	nbvertex_t mass_center() const;
	nbcoord_t total_energy() const;
	nbvertex_t last_total_impulce() const;
	nbvertex_t last_total_impulce_moment() const;
	nbvertex_t last_mass_center() const;
	nbcoord_t last_total_energy() const;

	nbcoord_t impulce_err() const;
	nbcoord_t impulce_moment_err() const;
	nbcoord_t energy_err() const;

	void dump();
};

#endif // NBODY_DATA_H
